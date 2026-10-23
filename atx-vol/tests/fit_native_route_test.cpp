// The native fit route (lane K4, r6 2026-10-23; QuoteSliceChannelConfig::deam_native_route).
//
// Pins: (1) a price on the exercise plateau inside the edge band is price-only, classified
// by C1's classifier, and never reaches Search; a continuation price in the band is
// identified on a certified bracket; (2) a refused family-native attempt goes to the bounded
// reference route, never to the seed leg; (3) the reference budget's exhaustion is a typed,
// counted refusal; (4) K3's PriceOnly / Interval acceptances map to C0's statuses without a
// re-solve; (5) a quote's bid and ask start from its mid's carry with no leg points
// (review-K2 MJ1) and typed outcomes bound a channel only by certified ends; (6) a fit on
// the route never runs Search, types every request, and its root audit finds no violation;
// band edges decided by bound agree with priced edges; the family grid serves a price only
// under its per-point bound.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/american_region.hpp"
#include "fitting/counters.hpp"
#include "fitting/deam_iv_cache.hpp"
#include "pricing/american_family_error.hpp"
#include "pricing/american_iv_batch_status.hpp"
#include "pricing/american_iv_premium.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;
namespace ib = iv_batch;
namespace ft = counters::fit_timing;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The fit's Premium options on the Screen seed tier with the Fast confirm kernel (the SPY
// speed arm's controls) and a native-route gate around them.
IvOpts premium_opts() {
  IvOpts o;
  o.strategy = IvStrategy::Premium;
  o.seed_profile = AlProfile::Screen;
  return o;
}

IvPremiumGate native_gate(IvReferenceBudget *budget) {
  IvPremiumGate gate;
  gate.confirm_opts = al_fast_profile(std::nullopt);
  gate.native_route = true;
  gate.reference_budget = budget;
  return gate;
}

// (1) The edge band, plateau: a deep in-the-money put quoted at its intrinsic sits on the
// exercise plateau (andersen_lake_classified: ExercisePlateau at kIvMin); the native route
// types it price-only after classifying, with no Search and no seed leg, while the default
// Premium route hands the same price to Search (RangeEdge).
TEST(FitNativeRoute, EdgeBandPriceOnThePlateauIsPriceOnlyAndNeverSearches) {
  const double S = 80.0, K = 100.0, T = 0.5, r = 0.05, q = 0.0;
  const auto cls = andersen_lake_classified(S, K, T, kIvMin, r, q, Side::Put, std::nullopt,
                                            AlExerciseRegion::Intrinsic);
  ASSERT_TRUE(cls);
  ASSERT_EQ(cls->cls, AlExerciseClass::ExercisePlateau);
  const double price = K - S; // the plateau's intrinsic
  IvReferenceBudget budget{4, 64};
  const IvPremiumGate gate = native_gate(&budget);
  IvPremiumStats st;
  const IvResult res =
      american_implied_vol_premium(price, S, K, T, r, q, Side::Put, premium_opts(), nullptr, st,
                                   &gate);
  EXPECT_EQ(res.status, IvStatus::NonMonotoneRegion);
  EXPECT_TRUE(std::isnan(res.iv));
  EXPECT_EQ(st.route, IvRouteOutcome::PriceOnlyPlateau);
  EXPECT_EQ(st.fallback, IvPremiumFallback::None);
  EXPECT_EQ(st.reference_evaluations, 1u); // kIvMin, classified on the plateau
  EXPECT_EQ(st.premium_solves, st.reference_evaluations); // nothing but the reference route
  EXPECT_EQ(st.reference_class, static_cast<std::uint8_t>(AlExerciseClass::ExercisePlateau));
  EXPECT_EQ(budget.calls, 1u);
  EXPECT_EQ(budget.evaluations, 1u);
  EXPECT_EQ(st.route_lo, 0.0);
  // Off the native route the same price leaves Premium for Search.
  IvPremiumGate legacy = gate;
  legacy.native_route = false;
  IvPremiumStats sl;
  static_cast<void>(american_implied_vol_premium(price, S, K, T, r, q, Side::Put, premium_opts(),
                                                 nullptr, sl, &legacy));
  EXPECT_EQ(sl.fallback, IvPremiumFallback::RangeEdge);
  EXPECT_EQ(sl.route, IvRouteOutcome::None);
}

// (1) The edge band, continuation: a deep out-of-the-money call priced inside the band (its
// premium below the accurate gate above the zero-vol value) is identified by the reference
// route on a bracket of the confirm kernel no wider than the requirement, containing the
// generating volatility.
TEST(FitNativeRoute, EdgeBandContinuationPriceIsIdentifiedOnACertifiedBracket) {
  const double S = 100.0, T = 0.25, r = 0.03, q = 0.01;
  double K = 0.0, sigma = 0.0, price = 0.0;
  for (const double k : {130.0, 140.0, 150.0, 160.0}) {
    for (const double s : {0.2, 0.25, 0.3, 0.35}) {
      const auto p = andersen_lake(S, k, T, s, r, q, Side::Call, al_fast_profile(std::nullopt));
      ASSERT_TRUE(p);
      const double band = iv_premium_edge_band_top(S, k, T, r, q, Side::Call, std::nullopt);
      if (K == 0.0 && *p > 1.0e-6 && *p < band) {
        K = k;
        sigma = s;
        price = *p;
      }
    }
  }
  ASSERT_GT(K, 0.0) << "fixture: no out-of-the-money call priced inside the edge band";
  IvReferenceBudget budget{4, 64};
  const IvPremiumGate gate = native_gate(&budget);
  IvPremiumStats st;
  const IvResult res = american_implied_vol_premium(price, S, K, T, r, q, Side::Call,
                                                    premium_opts(), nullptr, st, &gate);
  ASSERT_EQ(res.status, IvStatus::Ok) << static_cast<int>(st.route);
  EXPECT_EQ(st.route, IvRouteOutcome::ReferenceRoot);
  EXPECT_EQ(st.acceptance.kind, RootAcceptanceKind::Confirmed);
  EXPECT_EQ(st.acceptance.reason, RootAcceptanceReason::ConfirmBothEnds);
  EXPECT_LE(st.acceptance.hi - st.acceptance.lo, kIvRootRequirement);
  EXPECT_GE(res.iv, st.acceptance.lo);
  EXPECT_LE(res.iv, st.acceptance.hi);
  EXPECT_NEAR(res.iv, sigma, kIvRootRequirement);
  EXPECT_LE(st.reference_evaluations, kIvReferenceMaxEvaluations);
  EXPECT_EQ(st.fallback, IvPremiumFallback::None);
}

// (3) The reference budget: a request that finds it spent is refused (typed BudgetExhausted,
// counted in the budget) without an evaluation; one that spends the last evaluation stops
// there, also typed and counted - never a silent success, never a Search.
TEST(FitNativeRoute, ReferenceBudgetExhaustionIsATypedCountedRefusal) {
  const double S = 80.0, K = 100.0, T = 0.5, r = 0.05, q = 0.0;
  const double price = K - S + 1.0e-4; // inside the edge band, above the plateau
  {
    IvReferenceBudget spent{0, 64};
    const IvPremiumGate gate = native_gate(&spent);
    IvPremiumStats st;
    const IvResult res = american_implied_vol_premium(price, S, K, T, r, q, Side::Put,
                                                      premium_opts(), nullptr, st, &gate);
    EXPECT_EQ(res.status, IvStatus::NoConvergence);
    EXPECT_EQ(st.route, IvRouteOutcome::BudgetExhausted);
    EXPECT_EQ(st.reference_evaluations, 0u);
    EXPECT_EQ(spent.refused, 1u);
    EXPECT_EQ(spent.calls, 0u);
  }
  {
    IvReferenceBudget one{8, 1};
    const IvPremiumGate gate = native_gate(&one);
    IvPremiumStats st;
    const IvResult res = american_implied_vol_premium(price, S, K, T, r, q, Side::Put,
                                                      premium_opts(), nullptr, st, &gate);
    EXPECT_EQ(res.status, IvStatus::NoConvergence);
    EXPECT_EQ(st.route, IvRouteOutcome::BudgetExhausted);
    EXPECT_EQ(st.reference_evaluations, 1u);
    EXPECT_EQ(one.evaluations, 1u);
    EXPECT_EQ(one.refused, 1u);
    EXPECT_EQ(st.fallback, IvPremiumFallback::None);
  }
}

// (2) A family-native attempt the family cannot answer (the root above the family's sigma
// box) goes to the reference route from the box top the lane evaluated: identified there,
// with no seed-leg iterate (every Andersen-Lake solve is a reference evaluation) and no
// Search; the carry keeps the lane's bracket for the contract's next price.
TEST(FitNativeRoute, RefusedNativeAttemptGoesToTheReferenceRouteNotTheSeedLeg) {
  const double S = 100.0, T = 0.5, r = 0.04, q = 0.011;
  const AlExpiryGeometry g = prepare_al_geometry(T, r, q);
  AlSigmaQFamily put(g, Side::Put, S);
  ASSERT_TRUE(put.prepare(0.1, 0.35, 16, 0.010, 0.012, 4));
  const ValidatedErrorEnvelope env = build_family_error_envelope(put);
  IvReferenceBudget budget{4, 64};
  IvPremiumGate gate = native_gate(&budget);
  gate.qfamily = &put;
  gate.qfamily_bound = 1.0e-9;
  gate.qfamily_native = true;
  gate.seed_gate_kappa = 4.3e-5; // the SPY arms' routing gate
  const auto above = andersen_lake(S, 100.0, T, 0.8, r, q, Side::Put);
  ASSERT_TRUE(above);
  IvPremiumMemory memory;
  IvPremiumStats st;
  IvResult res;
  {
    const FamilyEnvelopeScope scope(&env);
    res = american_implied_vol_premium(*above, S, 100.0, T, r, q, Side::Put, premium_opts(),
                                       &memory, st, &gate);
  }
  ASSERT_EQ(res.status, IvStatus::Ok) << static_cast<int>(st.route);
  EXPECT_EQ(st.native_status, ib::InverseStatus::SigmaAboveBox);
  EXPECT_EQ(st.route, IvRouteOutcome::ReferenceRoot);
  EXPECT_TRUE(st.family_refused);
  EXPECT_GT(st.reference_evaluations, 0u);
  EXPECT_EQ(st.premium_solves, st.reference_evaluations); // no seed-leg cold iterate
  EXPECT_EQ(st.confirm_solves, 0u);
  EXPECT_FALSE(st.seed_routed);
  EXPECT_NEAR(res.iv, 0.8, kIvRootRequirement);
  EXPECT_LE(st.acceptance.hi - st.acceptance.lo, kIvRootRequirement);
  ASSERT_TRUE(memory.carry.valid);
  EXPECT_EQ(memory.carry.lo, put.sigma_hi());
  EXPECT_EQ(memory.points, 0u); // the reference route writes no leg point
  // Off the native route the same refusal goes to the seed leg (cold iterates).
  IvPremiumGate legacy = gate;
  legacy.native_route = false;
  IvPremiumStats sl;
  {
    const FamilyEnvelopeScope scope(&env);
    static_cast<void>(american_implied_vol_premium(*above, S, 100.0, T, r, q, Side::Put,
                                                   premium_opts(), nullptr, sl, &legacy));
  }
  EXPECT_EQ(sl.route, IvRouteOutcome::None);
  EXPECT_EQ(sl.reference_evaluations, 0u);
  EXPECT_GT(sl.premium_solves, 0u);
}

// (4) review-K3 For K4: an unaccepted family-native root's PriceOnly / Interval acceptance is
// a typed outcome (C0's low_vega / iv_interval), never re-solved; only a bracket that did not
// close (None) is sent on to the reference route.
TEST(FitNativeRoute, UnacceptedPriceOnlyAndIntervalAreTypedNotResolved) {
  EXPECT_EQ(native_unaccepted_outcome(RootAcceptanceKind::PriceOnly),
            IvRouteOutcome::PriceOnlyLowVega);
  EXPECT_EQ(native_unaccepted_outcome(RootAcceptanceKind::Interval), IvRouteOutcome::Interval);
  EXPECT_EQ(native_unaccepted_outcome(RootAcceptanceKind::None), IvRouteOutcome::None);
  EXPECT_EQ(native_unaccepted_outcome(RootAcceptanceKind::Confirmed), IvRouteOutcome::None);
  EXPECT_EQ(native_unaccepted_outcome(RootAcceptanceKind::Skipped), IvRouteOutcome::None);
}

// A scripted de-Am model for the view-level pins: the band edges at fixed prices, every
// Premium inversion scripted per target (its route outcome and result), recording what it
// was handed (the memory and its carry).
class RouteModel final : public DeAmModel {
public:
  struct Script {
    double target;
    IvRouteOutcome route;
    std::optional<double> iv;
    double lo = kNaN;
    double hi = kNaN;
  };
  struct Call {
    double target;
    bool memory;
    bool carried;
    bool native_route;
  };
  std::vector<Script> scripts;
  mutable std::vector<Call> calls;

  double effective_yield_of(double calendar_yield) const override { return calendar_yield; }
  double local_forward_of(double, double) const override { return 100.0; }
  double reference_yield() const override { return 0.0; }
  Result<double> price(const QuoteSliceQuote &, double sigma, double, DeAmKernel) const override {
    return Ok(sigma == kIvMin ? 0.01 : 1.0e3);
  }
  Result<double> implied_vol(const QuoteSliceQuote &, double, double, DeAmKernel) const override {
    return Err(ErrorCode::Unavailable, "the native route never runs Search");
  }
  Result<double> implied_vol_premium(const QuoteSliceQuote &, double target, double,
                                     IvPremiumMemory *memory, IvPremiumStats &stats,
                                     const DeAmPremiumControls &controls) const override {
    calls.push_back(Call{target, memory != nullptr, memory != nullptr && memory->carry.valid,
                         controls.native_route});
    stats = IvPremiumStats{};
    stats.fallback = IvPremiumFallback::None;
    if (memory != nullptr && !memory->carry.valid) {
      // The mid: a native attempt writes its carry and NO leg point.
      memory->carry.valid = true;
      memory->carry.lo = 0.1;
      memory->carry.hi = 0.4;
      memory->points = 0;
    }
    for (const Script &s : scripts) {
      if (s.target != target) continue;
      if (controls.native_route) { // off the native route nothing is typed
        stats.route = s.route;
        stats.route_lo = s.lo;
        stats.route_hi = s.hi;
      }
      if (s.iv) return Ok(*s.iv);
      return Err(ErrorCode::Unavailable, "scripted refusal");
    }
    return Err(ErrorCode::Unavailable, "unscripted target");
  }
  bool has_fast_kernel() const override { return true; }
  bool has_screen_kernel() const override { return true; }
  double seed_price_bound(double, DeAmKernel) const override { return 0.0; }
  double black_vega(const QuoteSliceQuote &, double, double) const override { return 1.0; }
};

DeAmConfirmControls native_controls(bool native) {
  DeAmConfirmControls c;
  c.family = QuoteSliceDeamFamily::Always;
  c.family_q = true;
  c.family_native = true;
  c.native_route = native;
  c.reference_max_calls = 16;
  c.reference_max_evaluations = 256;
  return c;
}

// (5) review-K2 MJ1: the mid's native attempt wrote a carry and no seed-leg point; on the
// native route the bid and the ask still start from a copy of the mid's memory (its carry),
// off it they start from scratch as before.
TEST(FitNativeRoute, BidAndAskStartFromTheMidCarryWithoutLegPoints) {
  const std::vector<QuoteSliceQuote> quotes = {{100.0, Side::Call, 1.0, 1.2}};
  for (const bool native : {true, false}) {
    SCOPED_TRACE(native ? "native route" : "family-native route");
    RouteModel model;
    model.scripts = {{1.1, IvRouteOutcome::Unresolved, std::nullopt},
                     {1.0, IvRouteOutcome::ReferenceRoot, 0.20},
                     {1.2, IvRouteOutcome::ReferenceRoot, 0.30}};
    DeAmIvCache cache;
    cache.ivs.assign(quotes.size(), DeAmQuoteIv{});
    QuoteSliceDeamDiagnostics work;
    DeAmIvView view(cache, model, quotes, QuoteSliceDeamProfile::Screen, work,
                    QuoteSliceDeamInversion::Premium, native_controls(native));
    const DeAmQuoteIv &band = view.band(0);
    ASSERT_EQ(model.calls.size(), 3u);
    EXPECT_EQ(model.calls[0].target, 1.1); // the mid first, from scratch
    EXPECT_TRUE(model.calls[0].memory);
    EXPECT_FALSE(model.calls[0].carried);
    EXPECT_TRUE(std::isnan(band.mid_iv));
    for (std::size_t c = 1; c < 3; ++c) {
      EXPECT_EQ(model.calls[c].memory, native) << c;
      EXPECT_EQ(model.calls[c].carried, native) << c;
      EXPECT_EQ(model.calls[c].native_route, native) << c;
    }
    EXPECT_DOUBLE_EQ(band.bid_iv, 0.20);
    EXPECT_DOUBLE_EQ(band.ask_iv, 0.30);
    EXPECT_EQ(band.mid_route, native ? IvRouteOutcome::Unresolved : IvRouteOutcome::None);
  }
}

// (5) A typed outcome that serves no IV bounds the channel only by a certified end: an
// interval's ends (bid: its lower end, ask: its upper end), never a price-only outcome's;
// the mid of an interval is not identified and carries the interval for C0's labels.
TEST(FitNativeRoute, TypedOutcomesBoundTheChannelOnlyByCertifiedEnds) {
  const std::vector<QuoteSliceQuote> quotes = {{100.0, Side::Call, 1.0, 1.2},
                                               {105.0, Side::Call, 2.0, 2.4}};
  RouteModel model;
  model.scripts = {{1.1, IvRouteOutcome::Interval, std::nullopt, 0.21, 0.29},
                   {1.0, IvRouteOutcome::Interval, std::nullopt, 0.18, 0.26},
                   {1.2, IvRouteOutcome::PriceOnlyLowVega, std::nullopt},
                   {2.2, IvRouteOutcome::ReferenceRoot, 0.25},
                   {2.0, IvRouteOutcome::BudgetExhausted, std::nullopt, 0.19, kNaN},
                   {2.4, IvRouteOutcome::PriceOnlyPlateau, std::nullopt, 0.0, kNaN}};
  DeAmIvCache cache;
  cache.ivs.assign(quotes.size(), DeAmQuoteIv{});
  QuoteSliceDeamDiagnostics work;
  DeAmIvView view(cache, model, quotes, QuoteSliceDeamProfile::Screen, work,
                  QuoteSliceDeamInversion::Premium, native_controls(true));
  const DeAmQuoteIv &a = view.band(0);
  EXPECT_TRUE(std::isnan(a.mid_iv));
  EXPECT_EQ(a.mid_route, IvRouteOutcome::Interval);
  EXPECT_DOUBLE_EQ(a.mid_lo, 0.21);
  EXPECT_DOUBLE_EQ(a.mid_hi, 0.29);
  EXPECT_DOUBLE_EQ(a.bid_iv, 0.18); // the interval's certified lower end
  EXPECT_TRUE(std::isinf(a.ask_iv)); // price-only: no upper information
  EXPECT_EQ(a.bid_route, IvRouteOutcome::Interval);
  EXPECT_EQ(a.ask_route, IvRouteOutcome::PriceOnlyLowVega);
  EXPECT_TRUE(a.well_posed);
  EXPECT_FALSE(view.mid_iv(0));
  const DeAmQuoteIv &b = view.band(1);
  EXPECT_DOUBLE_EQ(b.mid_iv, 0.25);
  EXPECT_DOUBLE_EQ(b.bid_iv, 0.19); // the budget stop's certified lower end
  EXPECT_TRUE(std::isinf(b.ask_iv)); // plateau: no certified upper end
  // The view counts every typed outcome once.
  EXPECT_EQ(work.price_only_outcomes, 2u);
  EXPECT_EQ(work.interval_outcomes, 2u);
  EXPECT_EQ(work.reference_budget_exhausted, 1u);
}

// An SPY-shaped chain (the vol-space suite's spy_chain): spot 670, strikes +-25% in 2.5%
// steps, a skewed smile priced with the engine the fitter inverts, strike-wise carry.
struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

Chain spy_chain(double years) {
  constexpr double kSpot = 670.0;
  constexpr double kRate = 0.04;
  constexpr double kYield = 0.012;
  Chain out;
  out.market = QuoteSliceMarket{kSpot, years, years, kRate, kYield, 0.0};
  for (int step = -10; step <= 10; ++step) {
    const double strike = std::round(kSpot * (1.0 + 0.025 * step));
    const double k = std::log(strike / kSpot);
    const double sigma = std::max(0.08, 0.16 - 0.30 * k + 0.80 * k * k);
    const double q = kYield + 0.004 * k;
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(kSpot, strike, years, sigma, kRate, q, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double half = std::max(0.02, 0.03 * *price);
      const bool otm = (side == Side::Call) == (k > 0.0);
      const double bid = otm && std::abs(k) > 0.2 ? 0.0 : std::max(*price - half, 0.0);
      QuoteSliceQuote quote{strike, side, bid, *price + half};
      quote.continuous_yield = q;
      out.quotes.push_back(quote);
    }
  }
  // A deep in-the-money put quoted at its intrinsic (+1e-11 at the mid): on the exercise
  // plateau, its mid is price-only on the native route.
  QuoteSliceQuote &deep = out.quotes.back();
  const double intrinsic = deep.strike - kSpot;
  deep.bid = intrinsic - 0.01;
  deep.ask = intrinsic + 0.01 + 2.0e-11;
  return out;
}

QuoteSliceConfig native_config(bool native) {
  QuoteSliceConfig config;
  config.dividend_model = QuoteSliceDividendModel::ContinuousYield;
  config.channel.repair_half_spreads = 0.5;
  config.channel.fit_space = QuoteSliceFitSpace::VolChannel;
  config.channel.deam_profile = QuoteSliceDeamProfile::Screen;
  config.channel.deam_cache = true;
  config.channel.deam_inversion = QuoteSliceDeamInversion::Premium;
  config.channel.deam_confirm_kernel = QuoteSliceDeamConfirmKernel::Fast;
  config.channel.deam_family = QuoteSliceDeamFamily::Always;
  config.channel.deam_family_q = true;
  config.channel.deam_family_native = true;
  config.channel.deam_native_route = native;
  config.grid_kernel = QuoteSliceGridKernel::Family;
  return config;
}

// (6) A fit on the native route: no Search solve, every request typed exactly once (no
// None outcome), every native lane outcome counted, the root audit (every typed root
// re-bisected on the accurate map) finds no violation, and the fit's outcome is the
// family-native route's. The deep put's mid on the plateau is typed price-only (plateau) in
// the cache and its row is UnidentifiableVolatility instead of an inversion failure.
TEST(FitNativeRoute, FitNeverSearchesTypesEveryRequestAndAuditsClean) {
  ft::arm_deam_audit(true);
  std::uint64_t requests = 0, typed = 0, lanes = 0, violations = 0, unresolved = 0;
  std::uint64_t search = 0, reference = 0, decided = 0, priced = 0;
  for (const double years : {0.15, 0.5}) {
    SCOPED_TRACE("T=" + std::to_string(years));
    const Chain c = spy_chain(years);
    // Every side admitted and no time-value floor: the deep put's mid reaches the inversion.
    QuoteSliceConfig plain = native_config(false);
    plain.side_policy = QuoteSliceSidePolicy::All;
    plain.min_time_value = 0.0;
    QuoteSliceConfig config = plain;
    config.channel.deam_native_route = true;
    deam_iv_memo_clear();
    const auto base = fit_quote_slice_channel(c.market, c.quotes, plain, nullptr);
    ASSERT_TRUE(base);
    deam_iv_memo_clear();
    ft::ChainRecorder rec;
    std::optional<QuoteSliceFit> fit;
    {
      const ft::ChainScope chain(&rec, 1);
      auto r = fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
      ASSERT_TRUE(r);
      fit = std::move(*r);
    }
    EXPECT_EQ(fit->fit_status, base->fit_status) << fit->fit_error << " | " << base->fit_error;
    // The plateau mid: typed in the chain's cache entry (C1's class, the plateau's interval
    // [0, NaN]); its row's input status is UnidentifiableVolatility, not an inversion failure.
    // (The row's price_only_reason / iv_lower / iv_upper are C0's served-answer fields.)
    const QuoteSliceRow &deep = fit->rows.back();
    bool reused = false;
    const auto cache =
        deam_iv_memo(deam_iv_key(*fit, c.quotes, config), c.quotes.size(), reused);
    ASSERT_TRUE(reused);
    const DeAmQuoteIv &e = cache->ivs.back();
    EXPECT_EQ(e.mid_route, IvRouteOutcome::PriceOnlyPlateau);
    EXPECT_EQ(e.mid_lo, 0.0);
    EXPECT_TRUE(std::isnan(e.mid_iv));
    EXPECT_TRUE(std::isnan(deep.market_iv));
    EXPECT_EQ(deep.quote_status, QuoteSliceQuoteStatus::UnidentifiableVolatility);
    std::printf("[K4 fit] T=%.2f deep put mid: native row status %s, family-native row %s\n",
                years, to_string(deep.quote_status), to_string(base->rows.back().quote_status));
    const ft::FitTimingCounters t = rec.result();
    requests += t.deam_premium;
    for (std::size_t o = 1; o < t.deam_route_outcome.size(); ++o) typed += t.deam_route_outcome[o];
    EXPECT_EQ(t.deam_route_outcome[0], 0u);
    for (const std::uint32_t n : t.deam_native_status) lanes += n;
    search += t.deam_al_by_reason[static_cast<std::size_t>(ft::DeamReason::Search)];
    reference += t.deam_reference_requests;
    violations += t.deam_audit_bracket_violations;
    unresolved += t.deam_audit_bracket_unresolved;
    decided += t.deam_edges_bound_decided;
    priced += t.deam_edges_priced;
    EXPECT_GE(t.deam_route_outcome[static_cast<std::size_t>(IvRouteOutcome::PriceOnlyPlateau)],
              1u);
  }
  ft::arm_deam_audit(false);
  deam_iv_memo_clear();
  std::printf("[K4 fit] requests %llu typed %llu lanes %llu search %llu reference %llu edges "
              "bound %llu priced %llu audit violations %llu unresolved %llu\n",
              static_cast<unsigned long long>(requests), static_cast<unsigned long long>(typed),
              static_cast<unsigned long long>(lanes), static_cast<unsigned long long>(search),
              static_cast<unsigned long long>(reference),
              static_cast<unsigned long long>(decided), static_cast<unsigned long long>(priced),
              static_cast<unsigned long long>(violations),
              static_cast<unsigned long long>(unresolved));
  EXPECT_GT(requests, 0u);
  EXPECT_EQ(typed, requests);
  EXPECT_EQ(lanes, requests);
  EXPECT_EQ(search, 0u);
  EXPECT_GT(decided, 0u);
  EXPECT_EQ(violations, 0u);
  EXPECT_EQ(unresolved, 0u);
}

// (6) Band edges decided by validated bounds agree with the edges priced on the accurate
// kernel: for every band of the chain, the mid-inside decision and the bid / ask inversions
// attempted are exactly those the accurate edge prices imply.
TEST(FitNativeRoute, BoundDecidedEdgesAgreeWithTheAccurateEdgePrices) {
  const Chain c = spy_chain(0.5);
  const QuoteSliceConfig config = native_config(true);
  deam_iv_memo_clear();
  const auto fit = fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
  ASSERT_TRUE(fit);
  bool reused = false;
  const std::shared_ptr<DeAmIvCache> cache =
      deam_iv_memo(deam_iv_key(*fit, c.quotes, config), c.quotes.size(), reused);
  ASSERT_TRUE(reused);
  std::size_t checked = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    const QuoteSliceQuote &quote = c.quotes[i];
    const DeAmQuoteIv &e = cache->ivs[i];
    if ((e.computed & DeAmQuoteIv::kBand) == 0u || !std::isnan(e.floor_price)) continue;
    const auto floor = andersen_lake(c.market.spot, quote.strike, c.market.variance_years, kIvMin,
                                     fit->diagnostics.effective_rate, e.q_effective, quote.side);
    const auto cap = andersen_lake(c.market.spot, quote.strike, c.market.variance_years, kIvMax,
                                   fit->diagnostics.effective_rate, e.q_effective, quote.side);
    ASSERT_TRUE(floor && cap);
    const double mid = 0.5 * quote.bid + 0.5 * quote.ask;
    const bool gates = quote.bid < *cap && quote.ask > *floor;
    EXPECT_EQ(e.mid_inside, quote.bid > 0.0 && mid > *floor && mid < *cap) << i;
    EXPECT_EQ(e.bid_route != IvRouteOutcome::None, gates && quote.bid > *floor) << i;
    EXPECT_EQ(e.ask_route != IvRouteOutcome::None, gates && quote.ask < *cap) << i;
    ++checked;
  }
  deam_iv_memo_clear();
  std::printf("[K4 edges] %zu bound-decided bands checked against the accurate edges\n",
              checked);
  EXPECT_GT(checked, 0u);
}

// (6) The family grid on the native route serves a node's family price only under its
// per-point bound (below the certificate allowance): the fit serves the same rows and
// statuses as the same keys with the Fast grid, every served price within the allowance
// 1e-8 + 1e-9 max(S, K) of the Fast grid's, and no family price served over its bound.
TEST(FitNativeRoute, FamilyGridServesOnlyUnderThePerPointBound) {
  std::size_t family_nodes = 0, compared = 0;
  double worst_bound = 0.0, max_excess = -std::numeric_limits<double>::infinity();
  for (const double years : {0.15, 0.5}) {
    SCOPED_TRACE("T=" + std::to_string(years));
    const Chain c = spy_chain(years);
    QuoteSliceConfig fast = native_config(true);
    fast.grid_kernel = QuoteSliceGridKernel::Fast;
    const QuoteSliceConfig family = native_config(true);
    deam_iv_memo_clear();
    const auto a = fit_quote_slice_channel(c.market, c.quotes, fast, nullptr);
    deam_iv_memo_clear();
    const auto b = fit_quote_slice_channel(c.market, c.quotes, family, nullptr);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->fit_status, b->fit_status);
    family_nodes += b->diagnostics.deam.grid_family_prices;
    worst_bound = std::max(worst_bound, b->diagnostics.deam.grid_max_family_bound_usd);
    EXPECT_EQ(a->diagnostics.deam.grid_family_prices, 0u);
    ASSERT_EQ(a->rows.size(), b->rows.size());
    for (std::size_t i = 0; i < a->rows.size(); ++i) {
      const QuoteSliceRow &x = a->rows[i];
      const QuoteSliceRow &y = b->rows[i];
      EXPECT_EQ(x.quote_status, y.quote_status) << i;
      EXPECT_EQ(x.evaluation_status, y.evaluation_status) << i;
      EXPECT_EQ(x.admitted, y.admitted) << i;
      if (std::isfinite(x.fitted_price) && std::isfinite(y.fitted_price)) {
        ++compared;
        const double allowance = 1.0e-8 + 1.0e-9 * std::max(c.market.spot, c.quotes[i].strike);
        const double move = std::abs(x.fitted_price - y.fitted_price);
        max_excess = std::max(max_excess, move - allowance);
        EXPECT_LT(move, allowance) << i;
      }
    }
  }
  deam_iv_memo_clear();
  std::printf("[K4 grid] family-priced grid nodes %zu (largest point bound %.3g USD), %zu served "
              "prices compared, max move less allowance %.3g\n",
              family_nodes, worst_bound, compared, max_excess);
  EXPECT_GT(compared, 0u);
  EXPECT_LT(worst_bound, 1.0e-8 + 1.0e-9 * 670.0 * 1.25);
}

} // namespace
} // namespace atx::vol
