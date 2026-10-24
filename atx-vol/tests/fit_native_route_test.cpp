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
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/american_region.hpp"
#include "atx/vol/api/simd/american_boundary_batch.hpp"
#include "atx/vol/api/simd/cpu.hpp"
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

// (1) The edge band past the plateau's edge (an SPY deep in-the-money put, 1.67 y, its mid one
// cent over the intrinsic): kIvMin and every sigma up to the plateau's edge (~0.145) price the
// intrinsic, and past it the premium grows like (sigma - sigma*)^2. The reference route
// bisects across the plateau (no chord through a flat point) and steps on sqrt(P - I), so the
// root is identified within the per-request cap - not left an Interval after 16 evaluations
// of a secant crawling across the plateau (the first SPY run: 225 such requests).
TEST(FitNativeRoute, EdgeBandRootPastThePlateausEdgeIsIdentifiedWithinTheCap) {
  const double S = 772.9949951171875, K = 905.0, T = 1.6703925132751465;
  const double r = 0.051124785095453262, q = 0.0068890395460387381;
  const double price = 132.01499938964844; // the quote's mid: intrinsic 132.005 + 0.01
  IvReferenceBudget budget{4, 64};
  const IvPremiumGate gate = native_gate(&budget);
  IvPremiumStats st;
  const IvResult res = american_implied_vol_premium(price, S, K, T, r, q, Side::Put,
                                                    premium_opts(), nullptr, st, &gate);
  ASSERT_EQ(res.status, IvStatus::Ok) << static_cast<int>(st.route);
  EXPECT_EQ(st.route, IvRouteOutcome::ReferenceRoot);
  EXPECT_LE(st.acceptance.hi - st.acceptance.lo, kIvRootRequirement);
  EXPECT_LE(st.reference_evaluations, kIvReferenceMaxEvaluations);
  const AlOpts fast = al_fast_profile(std::nullopt);
  const auto below = andersen_lake(S, K, T, st.acceptance.lo, r, q, Side::Put, fast);
  const auto above = andersen_lake(S, K, T, st.acceptance.hi, r, q, Side::Put, fast);
  ASSERT_TRUE(below && above);
  EXPECT_LT(*below, price); // the bracket's ends straddle the price on the confirm kernel
  EXPECT_GT(*above, price);
  std::printf("[K4 plateau edge] root %.10f in %u evaluations\n", res.iv,
              static_cast<unsigned>(st.reference_evaluations));
}

// (1) The reference route's range is Search's: a root above the seed box (kSigmaHi = 5) up to
// Search's expansion ceiling is identified, not typed AboveRange; and low vega is decided only
// at the root - a chord across a flat far tail (an out-of-the-money put priced near zero at
// the bisection point) is not the root's slope. Both from a dev200 chain the first native
// route run lost (JOBX, 3.4 days, a 199.5 % carry: its out-of-the-money asks imply 6-12 vol).
TEST(FitNativeRoute, ReferenceRangeIsSearchsAndLowVegaIsDecidedAtTheRoot) {
  const double T = 0.01, r = 0.03, q = 0.01;
  const auto high = andersen_lake(100.0, 150.0, T, 7.0, r, q, Side::Call,
                                  al_fast_profile(std::nullopt));
  ASSERT_TRUE(high);
  IvReferenceBudget budget{8, 128};
  const IvPremiumGate gate = native_gate(&budget);
  IvPremiumStats st;
  const IvResult res = american_implied_vol_premium(*high, 100.0, 150.0, T, r, q, Side::Call,
                                                    premium_opts(), nullptr, st, &gate);
  ASSERT_EQ(res.status, IvStatus::Ok) << static_cast<int>(st.route);
  EXPECT_EQ(st.route, IvRouteOutcome::ReferenceRoot);
  EXPECT_NEAR(res.iv, 7.0, kIvRootRequirement);
  // JOBX: S 4.525, the 4-strike put's ask 0.85 (no bid), 0.0094 y, r 4.49 %, q 199.5 %.
  const double S = 4.5250000953674316, K = 4.0, Tj = 0.0093978382647037506;
  const double rj = 0.044896867126226425, qj = 1.9952241182327271;
  IvPremiumStats sj;
  const IvResult rj_res = american_implied_vol_premium(0.85000002384185791, S, K, Tj, rj, qj,
                                                       Side::Put, premium_opts(), nullptr, sj,
                                                       &gate);
  ASSERT_EQ(rj_res.status, IvStatus::Ok) << static_cast<int>(sj.route);
  EXPECT_EQ(sj.route, IvRouteOutcome::ReferenceRoot);
  EXPECT_LE(sj.acceptance.hi - sj.acceptance.lo, kIvRootRequirement);
  std::printf("[K4 range] sigma 7 call: %.10f in %u evaluations; JOBX put ask: %.10f in %u\n",
              res.iv, static_cast<unsigned>(st.reference_evaluations), rj_res.iv,
              static_cast<unsigned>(sj.reference_evaluations));
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

Chain spy_chain(double years, double rate = 0.04, double yield = 0.012) {
  constexpr double kSpot = 670.0;
  Chain out;
  out.market = QuoteSliceMarket{kSpot, years, years, rate, yield, 0.0};
  for (int step = -10; step <= 10; ++step) {
    const double strike = std::round(kSpot * (1.0 + 0.025 * step));
    const double k = std::log(strike / kSpot);
    const double sigma = std::max(0.08, 0.16 - 0.30 * k + 0.80 * k * k);
    const double q = yield + 0.004 * k;
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(kSpot, strike, years, sigma, rate, q, side);
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

TEST(FitNativeRoute, IntrinsicBidDecidesTheFloorWithoutAnEndpointSolve) {
  for (const bool zero_bid : {false, true}) {
    for (const Side side : {Side::Put, Side::Call}) {
      SCOPED_TRACE(::testing::Message() << "zero bid " << zero_bid << " side "
                                        << static_cast<int>(side));
      Chain c = side == Side::Put ? spy_chain(0.5) : spy_chain(0.5, 0.012, 0.04);
      const std::size_t i = side == Side::Put
          ? (zero_bid ? 1u : c.quotes.size() - 1u)
          : (zero_bid ? c.quotes.size() - 2u : 0u);
      auto &quote = c.quotes[i];
      ASSERT_EQ(quote.side, side);
      const double intrinsic = std::max(side == Side::Put ? quote.strike - c.market.spot
                                                         : c.market.spot - quote.strike, 0.0);
      ASSERT_EQ(intrinsic == 0.0, zero_bid);
      quote.bid = intrinsic;
      // The deep-exercise family's upper bound is deliberately conservative. Clear it
      // so only the lower-bound comparison of bid versus floor is under test.
      quote.ask = intrinsic + (zero_bid ? 0.02 : 10.0);
      auto config = native_config(true);
      config.side_policy = QuoteSliceSidePolicy::All;
      config.min_time_value = 0.0;
      deam_iv_memo_clear();
      const auto fit = fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
      ASSERT_TRUE(fit);
      bool reused = false;
      const auto cache = deam_iv_memo(deam_iv_key(*fit, c.quotes, config), c.quotes.size(), reused);
      ASSERT_TRUE(reused);
      const auto &e = cache->ivs[i];
      ASSERT_TRUE(cache->families);
      const auto *family = cache->families->of_q(side);
      const auto *envelope = cache->families->q_envelope(side);
      ASSERT_TRUE(family && envelope);
      const auto bound = family_price_bounded(*family, *envelope, c.market.spot,
                                             quote.strike, family->sigma_lo(), e.q_effective);
      const double band_top = iv_premium_edge_band_top(
          c.market.spot, quote.strike, c.market.variance_years,
          fit->diagnostics.effective_rate, e.q_effective, side, fit->al_opts);
      ASSERT_GT(0.5 * quote.bid + 0.5 * quote.ask,
                std::max(bound.price + bound.bound_usd, band_top));
      const auto floor = andersen_lake(c.market.spot, quote.strike, c.market.variance_years,
                                      kIvMin, fit->diagnostics.effective_rate,
                                      e.q_effective, side);
      ASSERT_TRUE(floor);
      ASSERT_GE(*floor, intrinsic);
      ASSERT_LT(*floor, intrinsic + 0.01);
      EXPECT_TRUE(std::isnan(e.floor_price)); // certified without pricing either endpoint
      EXPECT_TRUE(std::isnan(e.cap_price));
      EXPECT_EQ(e.bid_route, IvRouteOutcome::None);
      EXPECT_EQ(e.mid_inside, !zero_bid);
      deam_iv_memo_clear();
    }
  }
}

// The added bound is the kernel's actual intrinsic clamp, with no assumptions on
// rate/yield signs. Unsupported double-boundary cases remain explicit errors.
TEST(FitNativeRoute, IntrinsicFloorBoundHoldsForBothSidesAndSignedCarry) {
  std::size_t checked = 0;
  for (const auto [r, q] : {std::pair{0.04, 0.012}, {0.012, 0.04}, {-0.01, 0.02},
                           {0.02, -0.01}, {-0.01, -0.03}, {-0.03, -0.01}, {0.0, 0.0}}) {
    for (const Side side : {Side::Put, Side::Call}) {
      for (const double K : {70.0, 99.0, 100.0, 101.0, 140.0}) {
        for (const double T : {0.02, 0.5, 3.0}) {
          const auto price = andersen_lake(100.0, K, T, kIvMin, r, q, side);
          if (!price) continue;
          const double floor = std::max(side == Side::Put ? K - 100.0 : 100.0 - K, 0.0);
          EXPECT_GE(*price, floor) << r << " " << q << " " << K << " " << T;
          ++checked;
        }
      }
    }
  }
  EXPECT_GE(checked, 180u);
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

// ── Lane K4b (r6): sub-clocks, the batched route, m1 / m2 / m5, the wall budget ──

// The inverse sub-clocks: under an armed chain timer every request's wall is charged, the
// family-native lane / the acceptance's family prices / the confirm solves are charged inside
// it (their sum never exceeds the requests' wall), and the band-edge wall per chain; an
// unarmed request reads no clock (IvPremiumStats::timed false, every sub-clock zero).
TEST(FitNativeRoute, InverseSubClocksAreChargedOnlyUnderAnArmedChainTimer) {
  const Chain c = spy_chain(0.5);
  const QuoteSliceConfig config = native_config(true);
  deam_iv_memo_clear();
  ft::ChainRecorder rec;
  {
    const ft::ChainScope chain(&rec, 1);
    ASSERT_TRUE(fit_quote_slice_channel(c.market, c.quotes, config, nullptr));
  }
  const ft::FitTimingCounters t = rec.result();
  std::uint64_t parts = 0;
  for (const std::uint64_t ns : t.deam_sub_ns) parts += ns;
  const auto sub = [&t](IvPremiumSubClock s) { return t.deam_sub_ns[static_cast<std::size_t>(s)]; };
  std::printf("[K4b subclock] request %.3f ms: lane %.3f family %.3f confirm %.3f leg %.3f "
              "reference %.3f; endpoints %.3f ms; grid family %.3f kernel %.3f ms\n",
              1.0e-6 * static_cast<double>(t.deam_sub_request_ns),
              1.0e-6 * static_cast<double>(sub(IvPremiumSubClock::Lane)),
              1.0e-6 * static_cast<double>(sub(IvPremiumSubClock::FamilyAccept)),
              1.0e-6 * static_cast<double>(sub(IvPremiumSubClock::Confirm)),
              1.0e-6 * static_cast<double>(sub(IvPremiumSubClock::Leg)),
              1.0e-6 * static_cast<double>(sub(IvPremiumSubClock::Reference)),
              1.0e-6 * static_cast<double>(t.deam_sub_endpoint_ns),
              1.0e-6 * static_cast<double>(t.deam_sub_grid_family_ns),
              1.0e-6 * static_cast<double>(t.deam_sub_grid_kernel_ns));
  EXPECT_GT(t.deam_premium, 0u);
  EXPECT_GT(t.deam_sub_request_ns, 0u);
  EXPECT_GT(sub(IvPremiumSubClock::Lane), 0u);
  EXPECT_GT(sub(IvPremiumSubClock::FamilyAccept), 0u);
  EXPECT_GT(sub(IvPremiumSubClock::Confirm), 0u);
  EXPECT_LE(parts, t.deam_sub_request_ns);
  EXPECT_GT(t.deam_sub_endpoint_ns, 0u);
  EXPECT_GT(t.deam_sub_grid_family_ns + t.deam_sub_grid_kernel_ns, 0u);
  // Unarmed: the Premium entry reads no clock.
  IvReferenceBudget budget{4, 64};
  const IvPremiumGate gate = native_gate(&budget);
  IvPremiumStats st;
  static_cast<void>(american_implied_vol_premium(2.0, 100.0, 100.0, 0.5, 0.03, 0.01, Side::Put,
                                                 premium_opts(), nullptr, st, &gate));
  EXPECT_FALSE(st.timed);
  for (const std::uint64_t ns : st.sub_ns) EXPECT_EQ(ns, 0u);
  deam_iv_memo_clear();
}

// Pins the process-wide SIMD dispatch for one test (restored on exit).
class IsaOverride {
public:
  explicit IsaOverride(simd::SimdIsa isa) noexcept : prev_(simd::simd_isa_override()) {
    simd::set_simd_isa_override(isa);
  }
  ~IsaOverride() { simd::set_simd_isa_override(prev_); }
  IsaOverride(const IsaOverride &) = delete;
  IsaOverride &operator=(const IsaOverride &) = delete;
  IsaOverride(IsaOverride &&) = delete;
  IsaOverride &operator=(IsaOverride &&) = delete;

private:
  simd::SimdIsa prev_;
};

bool same_bits(double a, double b) {
  return std::memcmp(&a, &b, sizeof(double)) == 0;
}

// One chain fitted on the native route with and without deam_native_batch, each from an empty
// memo; the rows, the chain cache entries and the batched fit's counters.
struct BatchPair {
  std::optional<QuoteSliceFit> plain;
  std::optional<QuoteSliceFit> batched;
  std::vector<DeAmQuoteIv> plain_ivs;
  std::vector<DeAmQuoteIv> batched_ivs;
  ft::FitTimingCounters plain_counts{};
  ft::FitTimingCounters batched_counts{};
};

BatchPair fit_batch_pair(const Chain &c,
                         QuoteSliceDeamConfirmKernel kernel = QuoteSliceDeamConfirmKernel::Fast) {
  BatchPair out;
  QuoteSliceConfig plain = native_config(true);
  plain.channel.deam_confirm_kernel = kernel;
  QuoteSliceConfig batched = plain;
  batched.channel.deam_native_batch = true;
  const auto run = [&c](const QuoteSliceConfig &config, std::optional<QuoteSliceFit> &fit,
                        std::vector<DeAmQuoteIv> &ivs, ft::FitTimingCounters &counts) {
    deam_iv_memo_clear();
    ft::ChainRecorder rec;
    {
      const ft::ChainScope chain(&rec, 1);
      auto r = fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
      if (!r) return;
      fit = std::move(*r);
    }
    counts = rec.result();
    bool reused = false;
    const auto cache = deam_iv_memo(deam_iv_key(*fit, c.quotes, config), c.quotes.size(), reused);
    if (reused) ivs = cache->ivs;
  };
  run(plain, out.plain, out.plain_ivs, out.plain_counts);
  run(batched, out.batched, out.batched_ivs, out.batched_counts);
  deam_iv_memo_clear();
  return out;
}

// Point rungs do not consume bid/ask inversions. A later channel rung must add
// exactly those deferred lanes, seeded from the original midpoint lane rather
// than the tighter carry its K3 acceptance may have written in the meantime.
TEST(FitNativeRoute, PointBatchDefersBandsAndLaterChannelKeepsTheOriginalLaneSeeds) {
  const IsaOverride scalar(simd::SimdIsa::ForceScalar);
  const Chain c = spy_chain(0.15);
  QuoteSliceConfig plain = native_config(true);
  QuoteSliceConfig batched = plain;
  batched.channel.deam_native_batch = true;
  deam_iv_memo_clear();
  const auto plain_point = fit_quote_slice(c.market, c.quotes, plain);
  const auto plain_channel = fit_quote_slice_channel(c.market, c.quotes, plain, nullptr);
  ASSERT_TRUE(plain_point && plain_channel);
  bool reused = false;
  const auto plain_cache = deam_iv_memo(deam_iv_key(*plain_channel, c.quotes, plain),
                                        c.quotes.size(), reused);
  ASSERT_TRUE(reused);
  const auto plain_ivs = plain_cache->ivs;
  deam_iv_memo_clear();

  ft::ChainRecorder point_counts;
  std::optional<QuoteSliceFit> point;
  {
    const ft::ChainScope timing(&point_counts, 1);
    auto fit = fit_quote_slice(c.market, c.quotes, batched);
    ASSERT_TRUE(fit);
    point = std::move(*fit);
  }
  const auto cache = deam_iv_memo(deam_iv_key(*point, c.quotes, batched),
                                  c.quotes.size(), reused);
  ASSERT_TRUE(reused);
  ASSERT_TRUE(cache->native_prepared);
  EXPECT_FALSE(cache->native_bands_prepared);
  ASSERT_EQ(cache->prepared.size(), c.quotes.size());
  std::size_t prepared_mids = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    prepared_mids += cache->prepared[i][0].lane.valid ? 1u : 0u;
    for (const std::size_t slot : {1u, 2u}) {
      EXPECT_FALSE(cache->prepared[i][slot].lane.valid) << i;
      EXPECT_FALSE(cache->prepared[i][slot].enclosure.valid) << i;
      EXPECT_FALSE(cache->prepared[i][slot].confirm.valid) << i;
    }
    EXPECT_TRUE(same_bits(point->rows[i].market_iv, plain_point->rows[i].market_iv)) << i;
    EXPECT_TRUE(same_bits(point->rows[i].fitted_price, plain_point->rows[i].fitted_price)) << i;
  }
  ASSERT_GT(prepared_mids, 0u);
  EXPECT_EQ(point_counts.result().deam_batch_lanes, prepared_mids);
  EXPECT_EQ(point_counts.result().channel_solves, 0u);
  const auto original_prepared = cache->prepared;

  ft::ChainRecorder band_counts;
  std::optional<QuoteSliceFit> channel;
  {
    const ft::ChainScope timing(&band_counts, 2);
    auto fit = fit_quote_slice_channel(c.market, c.quotes, batched, nullptr);
    ASSERT_TRUE(fit);
    channel = std::move(*fit);
  }
  EXPECT_TRUE(channel->diagnostics.deam.reused);
  EXPECT_TRUE(cache->native_bands_prepared);
  std::size_t prepared_edges = 0, checked_seeds = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    const auto &old_mid = original_prepared[i][0];
    const auto &mid = cache->prepared[i][0];
    EXPECT_EQ(mid.lane.valid, old_mid.lane.valid) << i;
    EXPECT_TRUE(same_bits(mid.lane.sigma, old_mid.lane.sigma)) << i;
    EXPECT_TRUE(same_bits(mid.lane.target, old_mid.lane.target)) << i;
    EXPECT_EQ(mid.enclosure.valid, old_mid.enclosure.valid) << i;
    EXPECT_TRUE(same_bits(mid.enclosure.enclosure.root, old_mid.enclosure.enclosure.root)) << i;
    EXPECT_EQ(mid.confirm.valid, old_mid.confirm.valid) << i;
    for (const std::size_t slot : {1u, 2u}) {
      const auto &edge = cache->prepared[i][slot].lane;
      if (!edge.valid) continue;
      ++prepared_edges;
      if (!old_mid.lane.valid) continue;
      const auto &quote = c.quotes[i];
      const double target = slot == 1u ? quote.bid : quote.ask;
      const auto carry = iv_native_lane_carry(old_mid.lane, 0.5 * quote.bid + 0.5 * quote.ask);
      const IvNativeBatchRequest request{target, c.market.spot, quote.strike,
          c.market.variance_years, c.market.rate, cache->ivs[i].q_effective, quote.side,
          batched.iv_tolerance, &carry};
      IvPremiumPrepared expected;
      IvNativeBatchWork work;
      iv_native_prepare_lanes(*cache->families->of_q(quote.side), nullptr,
          std::span<const IvNativeBatchRequest>(&request, 1),
          std::span<IvPremiumPrepared>(&expected, 1), false, work);
      ASSERT_TRUE(expected.lane.valid);
      EXPECT_TRUE(same_bits(edge.seed, expected.lane.seed)) << i << ':' << slot;
      EXPECT_TRUE(same_bits(edge.bracket_lo, expected.lane.bracket_lo)) << i << ':' << slot;
      EXPECT_TRUE(same_bits(edge.bracket_hi, expected.lane.bracket_hi)) << i << ':' << slot;
      EXPECT_TRUE(same_bits(edge.sigma, expected.lane.sigma)) << i << ':' << slot;
      EXPECT_EQ(edge.status, expected.lane.status) << i << ':' << slot;
      ++checked_seeds;
    }
    const auto &x = plain_ivs[i];
    const auto &y = cache->ivs[i];
    EXPECT_TRUE(same_bits(x.mid_iv, y.mid_iv)) << i;
    EXPECT_TRUE(same_bits(x.bid_iv, y.bid_iv)) << i;
    EXPECT_TRUE(same_bits(x.ask_iv, y.ask_iv)) << i;
    EXPECT_EQ(x.mid_route, y.mid_route) << i;
    EXPECT_EQ(x.bid_route, y.bid_route) << i;
    EXPECT_EQ(x.ask_route, y.ask_route) << i;
    EXPECT_EQ(x.well_posed, y.well_posed) << i;
    EXPECT_TRUE(same_bits(channel->rows[i].fitted_price, plain_channel->rows[i].fitted_price)) << i;
  }
  EXPECT_GT(checked_seeds, 0u);
  EXPECT_EQ(band_counts.result().deam_batch_lanes, prepared_edges);
  EXPECT_EQ(band_counts.result().deam_batch_lane_misses, 0u);
  RecordProperty("point_lanes", point_counts.result().deam_batch_lanes);
  RecordProperty("deferred_band_lanes", band_counts.result().deam_batch_lanes);
  RecordProperty("original_seeds_checked", checked_seeds);
  const auto again = fit_quote_slice_channel(c.market, c.quotes, batched, nullptr);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->diagnostics.deam.batch_lanes, 0u);
  EXPECT_EQ(again->diagnostics.deam.inversions, 0u);
  deam_iv_memo_clear();
}

// K4b deliverable 1 (bit identity): on the scalar map (ForceScalar) the batched route - every
// lane prepared in invert_many batches, consumed on bit-identical inputs - serves exactly what
// the unbatched native route serves: every row, every cached mid / bid / ask volatility and
// typed outcome, bit for bit; every request consumed its prepared lane (no miss).
TEST(FitNativeRoute, BatchedRouteOnTheScalarMapIsTheUnbatchedRouteBitForBit) {
  const IsaOverride scalar(simd::SimdIsa::ForceScalar);
  std::uint64_t lanes = 0, hits = 0, misses = 0, confirms = 0, rows = 0, entries = 0;
  for (const double years : {0.15, 0.5}) {
    SCOPED_TRACE("T=" + std::to_string(years));
    const BatchPair p = fit_batch_pair(spy_chain(years));
    ASSERT_TRUE(p.plain && p.batched);
    ASSERT_EQ(p.plain->rows.size(), p.batched->rows.size());
    EXPECT_EQ(p.plain->fit_status, p.batched->fit_status);
    for (std::size_t i = 0; i < p.plain->rows.size(); ++i) {
      const QuoteSliceRow &x = p.plain->rows[i];
      const QuoteSliceRow &y = p.batched->rows[i];
      EXPECT_EQ(x.quote_status, y.quote_status) << i;
      EXPECT_EQ(x.evaluation_status, y.evaluation_status) << i;
      EXPECT_TRUE(same_bits(x.market_iv, y.market_iv)) << i;
      EXPECT_TRUE(same_bits(x.fitted_iv, y.fitted_iv)) << i;
      EXPECT_TRUE(same_bits(x.fitted_price, y.fitted_price)) << i;
      ++rows;
    }
    ASSERT_EQ(p.plain_ivs.size(), p.batched_ivs.size());
    for (std::size_t i = 0; i < p.plain_ivs.size(); ++i) {
      const DeAmQuoteIv &x = p.plain_ivs[i];
      const DeAmQuoteIv &y = p.batched_ivs[i];
      EXPECT_TRUE(same_bits(x.mid_iv, y.mid_iv)) << i;
      EXPECT_TRUE(same_bits(x.bid_iv, y.bid_iv)) << i;
      EXPECT_TRUE(same_bits(x.ask_iv, y.ask_iv)) << i;
      EXPECT_EQ(x.mid_route, y.mid_route) << i;
      EXPECT_EQ(x.bid_route, y.bid_route) << i;
      EXPECT_EQ(x.ask_route, y.ask_route) << i;
      EXPECT_EQ(x.well_posed, y.well_posed) << i;
      ++entries;
    }
    EXPECT_EQ(p.plain_counts.deam_accept_by_kind, p.batched_counts.deam_accept_by_kind);
    EXPECT_EQ(p.plain_counts.deam_route_outcome, p.batched_counts.deam_route_outcome);
    EXPECT_EQ(p.plain_counts.deam_confirms, p.batched_counts.deam_confirms);
    EXPECT_EQ(p.plain_counts.deam_family_prices, p.batched_counts.deam_family_prices);
    EXPECT_EQ(p.batched_counts.deam_batch_avx2_calls, 0u);
    lanes += p.batched_counts.deam_batch_lanes;
    hits += p.batched_counts.deam_batch_lane_hits;
    misses += p.batched_counts.deam_batch_lane_misses;
    confirms += p.batched_counts.deam_batch_confirm_hits;
  }
  std::printf("[K4b batch scalar] %llu rows and %llu cache entries bit-identical; lanes %llu "
              "prepared, %llu consumed, %llu missed, confirm prices consumed %llu\n",
              static_cast<unsigned long long>(rows), static_cast<unsigned long long>(entries),
              static_cast<unsigned long long>(lanes), static_cast<unsigned long long>(hits),
              static_cast<unsigned long long>(misses), static_cast<unsigned long long>(confirms));
  EXPECT_GT(lanes, 0u);
  EXPECT_GT(hits, 0u);
  EXPECT_EQ(misses, 0u);
  EXPECT_EQ(confirms, 0u); // the confirm batch runs on the AVX2 kernel only
}

// K4b deliverables 1-2 (AVX2): the lanes run on the AVX2 kernel; on the ACCURATE confirm
// kernel the first confirm prices run on the AVX2 boundary batch (on the Fast confirm kernel
// they stay scalar: the batch is not faster than the scalar Fast solve - see
// iv_native_prepare_acceptance). Every typed outcome, acceptance kind and status is the
// unbatched route's, and the served values agree to the kernels' rounding class (far inside
// the 1e-7 root requirement and the certificate allowance).
TEST(FitNativeRoute, BatchedRouteOnAvx2KeepsEveryOutcomeToTheKernelsRoundingClass) {
  if (!simd::have_avx2()) GTEST_SKIP() << "host has no AVX2";
  const IsaOverride avx2(simd::SimdIsa::Auto);
  double worst_iv = 0.0, worst_row = 0.0, worst_price = 0.0;
  std::uint64_t avx2_calls = 0, confirm_hits = 0, misses = 0, fast_confirm_hits = 0;
  for (const auto kernel :
       {QuoteSliceDeamConfirmKernel::Fast, QuoteSliceDeamConfirmKernel::Accurate})
  for (const double years : {0.15, 0.5}) {
    const bool accurate = kernel == QuoteSliceDeamConfirmKernel::Accurate;
    SCOPED_TRACE(std::string(accurate ? "accurate" : "fast") + " confirms, T=" +
                 std::to_string(years));
    const BatchPair p = fit_batch_pair(spy_chain(years), kernel);
    ASSERT_TRUE(p.plain && p.batched);
    ASSERT_EQ(p.plain->rows.size(), p.batched->rows.size());
    ASSERT_EQ(p.plain_ivs.size(), p.batched_ivs.size());
    EXPECT_EQ(p.plain->fit_status, p.batched->fit_status);
    for (std::size_t i = 0; i < p.plain->rows.size(); ++i) {
      const QuoteSliceRow &x = p.plain->rows[i];
      const QuoteSliceRow &y = p.batched->rows[i];
      EXPECT_EQ(x.quote_status, y.quote_status) << i;
      EXPECT_EQ(x.evaluation_status, y.evaluation_status) << i;
      for (const auto &[a, b] : {std::pair{x.market_iv, y.market_iv},
                                 std::pair{x.fitted_iv, y.fitted_iv}}) {
        EXPECT_EQ(std::isfinite(a), std::isfinite(b)) << i;
        if (std::isfinite(a) && std::isfinite(b)) {
          worst_row = std::max(worst_row, std::abs(a - b));
          EXPECT_LE(std::abs(a - b), 0.01 * kIvRootRequirement) << i;
        }
      }
      if (std::isfinite(x.fitted_price) && std::isfinite(y.fitted_price)) {
        worst_price = std::max(worst_price, std::abs(x.fitted_price - y.fitted_price));
      }
    }
    for (std::size_t i = 0; i < p.plain_ivs.size(); ++i) {
      const DeAmQuoteIv &x = p.plain_ivs[i];
      const DeAmQuoteIv &y = p.batched_ivs[i];
      EXPECT_EQ(x.mid_route, y.mid_route) << i;
      EXPECT_EQ(x.bid_route, y.bid_route) << i;
      EXPECT_EQ(x.ask_route, y.ask_route) << i;
      for (const auto &[a, b] : {std::pair{x.mid_iv, y.mid_iv}, std::pair{x.bid_iv, y.bid_iv},
                                 std::pair{x.ask_iv, y.ask_iv}}) {
        EXPECT_EQ(std::isfinite(a), std::isfinite(b)) << i;
        if (std::isfinite(a) && std::isfinite(b)) {
          worst_iv = std::max(worst_iv, std::abs(a - b));
          EXPECT_LE(std::abs(a - b), 0.01 * kIvRootRequirement) << i;
        }
      }
    }
    EXPECT_EQ(p.plain_counts.deam_accept_by_kind, p.batched_counts.deam_accept_by_kind);
    EXPECT_EQ(p.plain_counts.deam_accept_by_reason, p.batched_counts.deam_accept_by_reason);
    EXPECT_EQ(p.plain_counts.deam_route_outcome, p.batched_counts.deam_route_outcome);
    EXPECT_EQ(p.plain_counts.deam_native_status, p.batched_counts.deam_native_status);
    EXPECT_EQ(p.plain_counts.deam_confirms, p.batched_counts.deam_confirms);
    avx2_calls += p.batched_counts.deam_batch_avx2_calls;
    (accurate ? confirm_hits : fast_confirm_hits) += p.batched_counts.deam_batch_confirm_hits;
    misses += p.batched_counts.deam_batch_lane_misses;
  }
  std::printf("[K4b batch avx2] kernel calls %llu, confirm prices consumed %llu (accurate "
              "confirms; fast %llu), lane misses %llu; max |d iv| cache %.3g rows %.3g, max |d "
              "fitted price| %.3g USD\n",
              static_cast<unsigned long long>(avx2_calls),
              static_cast<unsigned long long>(confirm_hits),
              static_cast<unsigned long long>(fast_confirm_hits),
              static_cast<unsigned long long>(misses), worst_iv, worst_row, worst_price);
  EXPECT_GT(avx2_calls, 0u);
  EXPECT_GT(confirm_hits, 0u);
  EXPECT_EQ(fast_confirm_hits, 0u);
  EXPECT_EQ(misses, 0u);
}

// K4b deliverable 2: the AVX2 boundary batch (a call through the McDonald-Schroder put map,
// as iv_native_prepare_acceptance runs it) prices the confirm kernel's scheme - the accurate
// one the batch serves, and Fast - within the class kIvBatchConfirmMarginRel max(P, 1) of the
// scalar Andersen-Lake kernel (its decisive margin), and the batch on the scalar route IS the
// scalar kernel bit for bit, puts and calls.
TEST(FitNativeRoute, BatchedConfirmPricesAreTheScalarKernelsToTheirClass) {
  for (const bool accurate : {true, false}) {
  SCOPED_TRACE(accurate ? "accurate scheme" : "fast profile");
  const std::optional<AlOpts> fast =
      accurate ? std::optional<AlOpts>{} : std::optional<AlOpts>{al_fast_profile(std::nullopt)};
  const double S = 670.0, r = 0.04;
  std::vector<double> bS, bK, bT, bsig, br, bq, ref;
  std::vector<Side> sides;
  std::vector<bool> american; // both maps run the American put solve (no European shortcut)
  for (const double T : {0.03, 0.4, 1.6}) {
    for (const double q : {0.0, 0.012, 0.035}) {
      for (const double m : {0.7, 0.9, 1.0, 1.1, 1.35}) {
        for (const double sigma : {0.08, 0.25, 0.7}) {
          for (const Side side : {Side::Put, Side::Call}) {
            const double K = std::round(S * m);
            const auto p = andersen_lake(S, K, T, sigma, r, q, side, fast);
            ASSERT_TRUE(p);
            const bool call = side == Side::Call;
            bS.push_back(call ? K : S);
            bK.push_back(call ? S : K);
            br.push_back(call ? q : r);
            bq.push_back(call ? r : q);
            bT.push_back(T);
            bsig.push_back(sigma);
            ref.push_back(*p);
            sides.push_back(side);
            american.push_back(!call || q > 0.0);
          }
        }
      }
    }
  }
  const std::size_t n = ref.size();
  std::vector<double> scalar(n), vec(n);
  static_cast<void>(simd::american_put_boundary_batch(bS.data(), bK.data(), bT.data(), bsig.data(),
                                                      br.data(), bq.data(), scalar.data(), n,
                                                      fast, simd::SimdIsa::ForceScalar));
  for (std::size_t i = 0; i < n; ++i) {
    // A call with q <= 0 is European on both maps (the Black-76 call vs its put image).
    if (american[i]) {
      EXPECT_TRUE(same_bits(scalar[i], ref[i])) << i << (sides[i] == Side::Call ? " call" : " put");
    } else {
      EXPECT_LE(std::abs(scalar[i] - ref[i]) / std::max(ref[i], 1.0), kIvBatchConfirmMarginRel)
          << i;
    }
  }
  if (!simd::have_avx2()) continue; // no AVX2: the scalar identity is what can be checked
  static_cast<void>(simd::american_put_boundary_batch(bS.data(), bK.data(), bT.data(), bsig.data(),
                                                      br.data(), bq.data(), vec.data(), n, fast,
                                                      simd::SimdIsa::ForceAvx2));
  double worst = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double rel = std::abs(vec[i] - ref[i]) / std::max(ref[i], 1.0);
    worst = std::max(worst, rel);
    EXPECT_LE(rel, kIvBatchConfirmMarginRel) << i;
  }
  std::printf("[K4b confirm batch] %s: %zu contracts: scalar route bit-identical, AVX2 max "
              "|dP| / max(P, 1) %.3g (margin %.1g)\n",
              accurate ? "accurate" : "fast", n, worst, kIvBatchConfirmMarginRel);
  }
}

// review-K4 m2: the reference route's bracket accepts a certified end only strictly inside
// the other certified end; a point on the wrong side of a certified end (a confirm kernel
// off the target map by more than its tested distance - a fake kernel here) is a crossing
// and leaves the bracket unchanged.
TEST(FitNativeRoute, ReferenceBracketRefusesACrossingEnd) {
  double lo = kIvMin, hi = 40.0, p_lo = kNaN, p_hi = kNaN;
  // An uncertified high end: a low side moves up to it.
  EXPECT_EQ(iv_reference_take(lo, hi, p_lo, p_hi, 0.2, 1.0, -1), IvReferenceTake::Lower);
  EXPECT_EQ(lo, 0.2);
  EXPECT_EQ(iv_reference_take(lo, hi, p_lo, p_hi, 0.3, 2.0, +1), IvReferenceTake::Upper);
  EXPECT_EQ(hi, 0.3);
  // The fake kernel now certifies the wrong sides.
  EXPECT_EQ(iv_reference_take(lo, hi, p_lo, p_hi, 0.35, 1.5, -1), IvReferenceTake::Crossed);
  EXPECT_EQ(iv_reference_take(lo, hi, p_lo, p_hi, 0.3, 1.5, -1), IvReferenceTake::Crossed);
  EXPECT_EQ(iv_reference_take(lo, hi, p_lo, p_hi, 0.15, 1.5, +1), IvReferenceTake::Crossed);
  EXPECT_EQ(iv_reference_take(lo, hi, p_lo, p_hi, 0.2, 1.5, +1), IvReferenceTake::Crossed);
  EXPECT_EQ(lo, 0.2);
  EXPECT_EQ(hi, 0.3);
  EXPECT_EQ(p_lo, 1.0);
  EXPECT_EQ(p_hi, 2.0);
  // Consistent points still narrow it; an undecided point (side 0) changes nothing.
  EXPECT_EQ(iv_reference_take(lo, hi, p_lo, p_hi, 0.25, 1.4, -1), IvReferenceTake::Lower);
  EXPECT_EQ(iv_reference_take(lo, hi, p_lo, p_hi, 0.27, 1.6, +1), IvReferenceTake::Upper);
  EXPECT_EQ(iv_reference_take(lo, hi, p_lo, p_hi, 0.26, 1.5, 0), IvReferenceTake::None);
  EXPECT_EQ(lo, 0.25);
  EXPECT_EQ(hi, 0.27);
}

// review-K4 m1: a reference-route price-only (low vega) outcome publishes only CERTIFIED
// bracket ends. A deep out-of-the-money call priced inside the edge band with a vega so low
// that the confirm kernel's tested bound cannot resolve 0.45 W: every finite published end
// is a certified side on the confirm kernel, and a top the search never certified is NaN
// (the ask side carries no upper information), never the unevaluated range top 40.
TEST(FitNativeRoute, LowVegaPriceOnlyPublishesOnlyCertifiedEnds) {
  const double S = 100.0, T = 0.1, r = 0.03, q = 0.01;
  IvReferenceBudget budget{4, 256};
  const IvPremiumGate gate = native_gate(&budget);
  bool found = false;
  int open_top = 0;
  for (const double K : {140.0, 150.0, 160.0, 175.0, 190.0, 210.0}) {
    for (const double sigma : {0.2, 0.25, 0.3, 0.35, 0.4, 0.5}) {
      const auto p = andersen_lake(S, K, T, sigma, r, q, Side::Call, al_fast_profile(std::nullopt));
      ASSERT_TRUE(p);
      const double band = iv_premium_edge_band_top(S, K, T, r, q, Side::Call, std::nullopt);
      if (!(*p > 1.0e-10 && *p < 1.0e-7 && *p < band)) continue;
      IvPremiumStats st;
      static_cast<void>(american_implied_vol_premium(*p, S, K, T, r, q, Side::Call, premium_opts(),
                                                     nullptr, st, &gate));
      if (st.route != IvRouteOutcome::PriceOnlyLowVega) continue;
      found = true;
      std::printf("[K4b m1] K %.0f sigma %.2f price %.3g: price-only low vega, ends [%g, %g]\n", K,
                  sigma, *p, st.route_lo, st.route_hi);
      // Never the unevaluated range top (the pre-m1 route published kSigmaHiCap = 40 there).
      EXPECT_NE(st.route_hi, 40.0);
      const double eps = al_profile_price_bound(AlProfile::Fast, *p, T);
      const AlOpts fast = al_fast_profile(std::nullopt);
      if (std::isfinite(st.route_lo)) { // a certified low side on the confirm kernel
        const auto c = andersen_lake(S, K, T, st.route_lo, r, q, Side::Call, fast);
        ASSERT_TRUE(c);
        EXPECT_LE(*c + eps, *p);
      }
      if (std::isfinite(st.route_hi)) { // a certified high side
        const auto c = andersen_lake(S, K, T, st.route_hi, r, q, Side::Call, fast);
        ASSERT_TRUE(c);
        EXPECT_GE(*c - eps, *p);
      } else {
        ++open_top;
      }
    }
  }
  std::printf("[K4b m1] price-only low-vega outcomes with no certified top: %d\n", open_top);
  ASSERT_TRUE(found) << "fixture: no low-vega edge-band call found";
  EXPECT_GT(open_top, 0); // the case review-K4 m1 names (red before: 40 published)
}

// K4b: the reference route's wall budget (count AND time): a chain whose reference wall is
// spent refuses the next request typed BudgetExhausted and counts it (refused_wall); a roomy
// wall budget serves the same request and is charged its wall.
TEST(FitNativeRoute, ReferenceWallBudgetIsATypedCountedRefusal) {
  const double S = 100.0, T = 0.25, r = 0.03, q = 0.01;
  double K = 0.0, price = 0.0;
  for (const double k : {130.0, 140.0, 150.0, 160.0}) {
    for (const double s : {0.2, 0.25, 0.3, 0.35}) {
      const auto p = andersen_lake(S, k, T, s, r, q, Side::Call, al_fast_profile(std::nullopt));
      ASSERT_TRUE(p);
      const double band = iv_premium_edge_band_top(S, k, T, r, q, Side::Call, std::nullopt);
      if (K == 0.0 && *p > 1.0e-6 && *p < band) {
        K = k;
        price = *p;
      }
    }
  }
  ASSERT_GT(K, 0.0) << "fixture: no out-of-the-money call priced inside the edge band";
  IvReferenceBudget spent{4, 64};
  spent.max_wall_ns = 1000;
  spent.wall_ns = 1000;
  const IvPremiumGate refused_gate = native_gate(&spent);
  IvPremiumStats sr;
  const IvResult rr = american_implied_vol_premium(price, S, K, T, r, q, Side::Call,
                                                   premium_opts(), nullptr, sr, &refused_gate);
  EXPECT_NE(rr.status, IvStatus::Ok);
  EXPECT_EQ(sr.route, IvRouteOutcome::BudgetExhausted);
  EXPECT_EQ(sr.reference_evaluations, 0u);
  EXPECT_EQ(spent.refused, 1u);
  EXPECT_EQ(spent.refused_wall, 1u);
  EXPECT_EQ(spent.calls, 0u);
  IvReferenceBudget roomy{4, 64};
  roomy.max_wall_ns = 60'000'000'000ull;
  const IvPremiumGate served_gate = native_gate(&roomy);
  IvPremiumStats ss;
  const IvResult rs = american_implied_vol_premium(price, S, K, T, r, q, Side::Call,
                                                   premium_opts(), nullptr, ss, &served_gate);
  EXPECT_EQ(rs.status, IvStatus::Ok);
  EXPECT_EQ(ss.route, IvRouteOutcome::ReferenceRoot);
  EXPECT_GT(roomy.wall_ns, 0u);
  EXPECT_EQ(roomy.refused_wall, 0u);
}

// review-K4 m5: on the native route a request whose side carries no family is answered by
// the bounded reference route (from the Black-76 IV) - no seed-leg iterate, no confirm of a
// seed-tier root - when deam_native_no_leg asks; without it, the K4 seed leg.
TEST(FitNativeRoute, NoFamilyRequestGoesToTheReferenceRouteWithoutTheSeedLeg) {
  const double S = 100.0, K = 100.0, T = 0.5, r = 0.04, q = 0.011, sigma = 0.25;
  const auto p = andersen_lake(S, K, T, sigma, r, q, Side::Put, al_fast_profile(std::nullopt));
  ASSERT_TRUE(p);
  IvReferenceBudget b1{4, 64};
  const IvPremiumGate leg_gate = native_gate(&b1);
  IvPremiumStats sl;
  const IvResult rl = american_implied_vol_premium(*p, S, K, T, r, q, Side::Put, premium_opts(),
                                                   nullptr, sl, &leg_gate);
  ASSERT_EQ(rl.status, IvStatus::Ok);
  EXPECT_EQ(sl.route, IvRouteOutcome::LegRoot);
  EXPECT_GT(sl.premium_solves, sl.confirm_solves + sl.reference_evaluations); // leg iterates
  IvReferenceBudget b2{4, 64};
  IvPremiumGate ref_gate = native_gate(&b2);
  ref_gate.no_family_reference = true;
  IvPremiumStats sr;
  const IvResult rr = american_implied_vol_premium(*p, S, K, T, r, q, Side::Put, premium_opts(),
                                                   nullptr, sr, &ref_gate);
  ASSERT_EQ(rr.status, IvStatus::Ok);
  EXPECT_EQ(sr.route, IvRouteOutcome::ReferenceRoot);
  EXPECT_EQ(sr.premium_solves, sr.reference_evaluations); // nothing but the reference route
  EXPECT_EQ(sr.confirm_solves, 0u);
  EXPECT_FALSE(sr.seed_routed);
  EXPECT_EQ(sr.acceptance.kind, RootAcceptanceKind::Confirmed);
  EXPECT_NEAR(rr.iv, sigma, kIvRootRequirement);
  EXPECT_NEAR(rr.iv, rl.iv, 2.0 * kIvRootRequirement);
}

// review-K4 m3 / K4b: the lane-vega hook reaches the enclosure step. Without a covering
// envelope (no envelope scope here) the enclosure cannot skip, so on the native route the
// acceptance places its confirm with the lane's fused vega and spends no family price beyond
// the lane's; off the route (K3's family-native acceptance) the enclosure's two family prices
// measure that chord. Both roots are confirmed within the requirement.
TEST(FitNativeRoute, LaneVegaHookReplacesTheUncoveredEnclosuresChord) {
  const double S = 100.0, K = 100.0, T = 0.5, r = 0.04, q = 0.011, sigma = 0.2;
  const AlExpiryGeometry g = prepare_al_geometry(T, r, q);
  AlSigmaQFamily put(g, Side::Put, S);
  ASSERT_TRUE(put.prepare(0.1, 0.35, 16, 0.010, 0.012, 4));
  const auto p = andersen_lake(S, K, T, sigma, r, q, Side::Put);
  ASSERT_TRUE(p);
  const auto run = [&](bool native_route, IvPremiumStats &st) {
    IvReferenceBudget budget{4, 64};
    IvPremiumGate gate = native_gate(&budget);
    gate.native_route = native_route;
    gate.qfamily = &put;
    gate.qfamily_native = true;
    return american_implied_vol_premium(*p, S, K, T, r, q, Side::Put, premium_opts(), nullptr,
                                        st, &gate);
  };
  IvPremiumStats sn, sk;
  const IvResult rn = run(true, sn);
  const IvResult rk = run(false, sk);
  ASSERT_EQ(rn.status, IvStatus::Ok);
  ASSERT_EQ(rk.status, IvStatus::Ok);
  EXPECT_EQ(sn.native_status, ib::InverseStatus::Converged);
  EXPECT_TRUE(sn.lane_vega_used);
  EXPECT_EQ(sn.family_prices, sn.native_evaluations); // no enclosure prices
  EXPECT_EQ(sn.acceptance.kind, RootAcceptanceKind::Confirmed);
  EXPECT_FALSE(sk.lane_vega_used);
  EXPECT_EQ(sk.family_prices, sk.native_evaluations + 2u); // the enclosure's chord
  EXPECT_EQ(sk.acceptance.kind, RootAcceptanceKind::Confirmed);
  EXPECT_NEAR(rn.iv, sigma, kIvRootRequirement);
  EXPECT_NEAR(rn.iv, rk.iv, kIvRootRequirement);
}

// K4b deliverable 3 (review-K4 m6): per-contract error metadata of the certification grid.
// On the native route (family grid, per-point bound) every served row carries its grid
// node's bound on |grid price - the accurate model| - under the certificate's allowance by
// construction (a family node's bound decided it; a Fast node's is the kernel's tested
// bound); off the route none is published (NaN).
TEST(FitNativeRoute, GridCarriesPerContractErrorMetadataOnTheRoute) {
  std::size_t native_served = 0;
  for (const double years : {0.15, 0.5})
  for (const bool native : {false, true}) {
    SCOPED_TRACE(std::string(native ? "native route" : "family-native") + ", T=" +
                 std::to_string(years));
    const Chain c = spy_chain(years);
    deam_iv_memo_clear();
    const auto fit = fit_quote_slice_channel(c.market, c.quotes, native_config(native), nullptr);
    ASSERT_TRUE(fit);
    if (!native) {
      for (const QuoteSliceRow &row : fit->rows) EXPECT_TRUE(std::isnan(row.grid_bound_usd));
      continue;
    }
    if (fit->fit_status != QuoteSliceFitStatus::Success) {
      // No certified grid (this fixture at 0.5 y fails its certificate): nothing published.
      for (const QuoteSliceRow &row : fit->rows) EXPECT_TRUE(std::isnan(row.grid_bound_usd));
      continue;
    }
    std::size_t carried = 0, served = 0, family = 0;
    double worst = 0.0;
    for (std::size_t i = 0; i < fit->rows.size(); ++i) {
      const QuoteSliceRow &row = fit->rows[i];
      if (row.evaluation_status != QuoteSliceEvaluationStatus::Success) continue;
      ++served;
      ASSERT_TRUE(std::isfinite(row.grid_bound_usd)) << i;
      ++carried;
      worst = std::max(worst, row.grid_bound_usd);
      if (row.grid_bound_usd > 1.0e-12 * std::max(row.fitted_price, 1.0)) ++family;
      EXPECT_GE(row.grid_bound_usd, 0.0) << i;
      EXPECT_LT(row.grid_bound_usd,
                1.0e-8 + 1.0e-9 * std::max(c.market.spot, c.quotes[i].strike))
          << i;
    }
    EXPECT_EQ(carried, served);
    EXPECT_GT(family, 0u); // some nodes are the family's, with their per-point bound
    native_served += served;
    std::printf("[K4b grid metadata] T=%.2f: %zu served rows carry a bound (%zu above Fast's), "
                "max %.3g USD\n",
                years, carried, family, worst);
  }
  EXPECT_GT(native_served, 0u);
  deam_iv_memo_clear();
}

// PM item A17 (K5's four-cohort K3 audit, dev200b; pre-dates K5 on the root 746154d6): the
// reference route served a root 0.138 vol from the accurate map's. dev200b chain
// 452a6746d690b21d (BABO, 0.42 y, q_eff 0.417), the 3 call's mid 4.15 = the intrinsic
// (S - K = 4.1499998569) + 1.19e-7. The route certifies its bracket on the CLASSIFIED map
// (Intrinsic region: the intrinsic exactly on the exercise side of B(T)); its low end there
// is an ExercisePlateau point. The accurate map the audit re-bisects (andersen_lake,
// KernelFloor: euro + premium floored at the intrinsic) prices that end above the intrinsic
// by more than the price's 1.19e-7 premium - the two conventions disagree near the plateau's
// edge by more than the confirm kernel's eps_c. A served root's every end must carry its
// side on the accurate map too: here the price is within the conventions' distance of the
// plateau, so it is typed price-only (plateau), never served.
TEST(FitNativeRoute, PlateauEndOfAReferenceBracketIsReDecidedOnTheAccurateMap) {
  const double S = 7.1499998569488525, K = 3.0, T = 0.4237929582595825;
  const double r = 0.04721422493457794, q = 0.41747474283973884;
  const double price = 4.1499999761581421; // the quote's mid (bid 3.30 / ask 5)
  for (const double sigma : {0.9, 1.1, 1.149, 1.2, 1.287, 1.3}) {
    const auto a = andersen_lake(S, K, T, sigma, r, q, Side::Call, std::nullopt);
    const auto c = andersen_lake_classified(S, K, T, sigma, r, q, Side::Call,
                                            al_fast_profile(std::nullopt),
                                            AlExerciseRegion::Intrinsic);
    ASSERT_TRUE(a && c);
    std::printf("[K4b A17] sigma %.3f: accurate - intrinsic %.3e, classified %.3e (class %d)\n",
                sigma, *a - (S - K), c->price - (S - K), static_cast<int>(c->cls));
  }
  IvReferenceBudget budget{4, 64};
  const IvPremiumGate gate = native_gate(&budget);
  IvPremiumStats st;
  const IvResult res = american_implied_vol_premium(price, S, K, T, r, q, Side::Call,
                                                    premium_opts(), nullptr, st, &gate);
  std::printf("[K4b A17] route %d status %d iv %.10f bracket [%.10f, %.10f] evaluations %u\n",
              static_cast<int>(st.route), static_cast<int>(res.status), res.iv,
              st.acceptance.lo, st.acceptance.hi,
              static_cast<unsigned>(st.reference_evaluations));
  if (res.status == IvStatus::Ok) {
    // K3's audit: the accurate map straddles the price within the requirement of the root.
    const auto below = andersen_lake(S, K, T, res.iv - kIvRootRequirement, r, q, Side::Call,
                                     std::nullopt);
    const auto above = andersen_lake(S, K, T, res.iv + kIvRootRequirement, r, q, Side::Call,
                                     std::nullopt);
    ASSERT_TRUE(below && above);
    EXPECT_LE(*below, price) << "served " << res.iv << " is not the accurate map's root";
    EXPECT_GE(*above, price) << "served " << res.iv << " is not the accurate map's root";
  }
  EXPECT_EQ(st.route, IvRouteOutcome::PriceOnlyPlateau);
  EXPECT_NE(res.status, IvStatus::Ok);
  EXPECT_EQ(st.acceptance.kind, RootAcceptanceKind::None); // nothing for the audit to serve
}

// review-K4 m8: a family side the adaptive rule asked for and the model built none of is
// counted once, by its typed reason, so the reasons sum to the sides asked for less those
// built (the fail-on-revert: untyped, every refusal is lost from the count). A two-day expiry
// is below kAlSigmaFamilyMinTau (ShortExpiry on every side asked for); a chain with negative
// carry refuses its call side (CallNoCarry: a call with q <= 0 never exercises early).
TEST(FitNativeRoute, FamilyRefusalsAreTypedByReason) {
  const auto run = [](const Chain &c) {
    QuoteSliceConfig config = native_config(true);
    config.side_policy = QuoteSliceSidePolicy::All;
    deam_iv_memo_clear();
    ft::ChainRecorder rec;
    {
      const ft::ChainScope chain(&rec, 1);
      const auto fit = fit_quote_slice_channel(c.market, c.quotes, config, nullptr);
      static_cast<void>(fit);
    }
    deam_iv_memo_clear();
    return rec.result();
  };
  const auto refused = [](const ft::FitTimingCounters &t) {
    std::uint32_t n = 0;
    for (const std::uint32_t k : t.deam_family_refused_by_reason) n += k;
    return n;
  };
  const auto at = [](const ft::FitTimingCounters &t, DeAmFamilyRefusal why) {
    return t.deam_family_refused_by_reason[static_cast<std::size_t>(why)];
  };
  Chain carry_less;
  {
    constexpr double kSpot = 670.0, kRate = 0.04, kYield = -0.005, kYears = 0.5;
    carry_less.market = QuoteSliceMarket{kSpot, kYears, kYears, kRate, kYield, 0.0};
    for (int step = -8; step <= 8; ++step) {
      const double strike = std::round(kSpot * (1.0 + 0.025 * step));
      const double k = std::log(strike / kSpot);
      const double sigma = std::max(0.08, 0.16 - 0.30 * k + 0.80 * k * k);
      for (Side side : {Side::Call, Side::Put}) {
        const auto price = andersen_lake(kSpot, strike, kYears, sigma, kRate, kYield, side);
        ASSERT_TRUE(price);
        const double half = std::max(0.02, 0.03 * *price);
        QuoteSliceQuote quote{strike, side, std::max(*price - half, 0.0), *price + half};
        quote.continuous_yield = kYield;
        carry_less.quotes.push_back(quote);
      }
    }
  }
  const ft::FitTimingCounters spy = run(spy_chain(0.5));
  const ft::FitTimingCounters brief = run(spy_chain(2.0 / 365.0));
  const ft::FitTimingCounters flat = run(carry_less);
  for (const ft::FitTimingCounters *t : {&spy, &brief, &flat}) {
    std::printf("[K4b m8] asked %u built %u refused by reason [", t->deam_family_sides_requested,
                t->deam_family_builds);
    for (const std::uint32_t k : t->deam_family_refused_by_reason) std::printf(" %u", k);
    std::printf(" ]\n");
    EXPECT_GT(t->deam_family_sides_requested, 0u);
    EXPECT_EQ(t->deam_family_sides_requested, t->deam_family_builds + refused(*t));
  }
  EXPECT_GT(spy.deam_family_builds, 0u);
  EXPECT_EQ(brief.deam_family_builds, 0u);
  EXPECT_EQ(at(brief, DeAmFamilyRefusal::ShortExpiry), brief.deam_family_sides_requested);
  EXPECT_GE(at(flat, DeAmFamilyRefusal::CallNoCarry), 1u);
  EXPECT_EQ(at(flat, DeAmFamilyRefusal::NotOffered), 0u);
}

} // namespace
} // namespace atx::vol
