#pragma once

// risk_synthetic_market — the data-free market + book fixture shared by the risk-engine
// suites (risk_market_state / risk_book / risk_kernel / risk_pnl_explain / risk_stream).
//
// Modeled on bench/support/synth_book.hpp (mixed eSSVI / ConvexDense PricedSurfaces
// built from parameters, no fit) and tests/pnl_greeks_consistency_test.cpp (coherent
// carry: F = S exp((r - q) T), q > 0 so BOTH sides carry early-exercise premium and
// both sides build sigma families). Three underliers, four expiries, a 61-leg option
// book over 60 unique contracts plus three share legs, and a deterministic state
// generator that moves spot / vol level / skew / rate / time.
//
// THE TIME GRID. Every state instant and every expiry sits on a grid of
// kUnitNs = kCalendarYearNs / 2^14 (an exact integer), so every year-fraction is an
// exact dyadic m / 16384 and T_b - dt == T_t to the bit. PortfolioPricer rolls T by
// that subtraction while the risk engine measures T_t from the shifted state, so the
// grid is what lets the oracle-parity test compare the two at all.
//
// The surfaces of a moved state are "the same expiries one step later": slice pillars
// sit at the expiries measured from the state's own instant, so a contract's T is a
// pillar T at every state.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/fitting/dense_slice.hpp"
#include "atx/vol/api/fitting/surface_parity.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "atx/vol/api/risk/risk_book.hpp"

namespace atx::vol::risk_test {

inline constexpr std::int64_t kT0 = 1700000000000000000LL;
inline constexpr std::int64_t kUnitNs = 1926123046875LL; // kCalendarYearNs / 16384
static_assert(static_cast<double>(kUnitNs) * 16384.0 == kCalendarYearNs,
              "the time grid unit must divide the calendar year exactly");
inline constexpr std::int64_t kDayUnits = 45; // ~1.003 calendar days
inline constexpr double kRate = 0.043;
// T ~ 0.050, 0.150, 0.300, 0.600 at kT0.
inline constexpr std::array<std::int64_t, 4> kExpiryUnits{819, 2458, 4915, 9830};

struct UnderlierSpec {
  std::uint32_t uid;
  double spot;
  double q;
  bool convex;      // ConvexDenseCurve slices (else eSSVI)
  double vol_short; // ATM vol at the short end
  double vol_long;  // ATM vol at the long end
  double skew;      // eSSVI rho / convex d(sigma)/d(ln K/F)
};

inline constexpr std::array<UnderlierSpec, 3> kUnderliers{{
    {1u, 100.0, 0.015, false, 0.30, 0.22, -0.40},
    {2u, 250.0, 0.010, true, 0.26, 0.20, -0.15},
    {3u, 50.0, 0.030, false, 0.38, 0.28, -0.25},
}};

[[nodiscard]] inline std::int64_t instant_ns(std::int64_t units) noexcept {
  return kT0 + units * kUnitNs;
}
[[nodiscard]] inline std::int64_t expiry_ns(std::size_t i) noexcept {
  return instant_ns(kExpiryUnits[i]);
}
[[nodiscard]] inline double years(std::int64_t units) noexcept {
  return static_cast<double>(units) / 16384.0;
}

// Short-end-rich ATM term structure: sigma at a FIXED strike moves as T rolls down,
// so the ContractOwn and CommonBaseMaturity vol-move conventions genuinely differ.
[[nodiscard]] inline double atm_vol(const UnderlierSpec &u, double T) noexcept {
  return u.vol_long + (u.vol_short - u.vol_long) * std::exp(-T / 0.15);
}

// One state's displacement from the base market.
struct StateMove {
  std::int64_t time_units{0};             // instant = kT0 + time_units * kUnitNs
  std::array<double, 3> spot_ret{0.0, 0.0, 0.0}; // relative spot move per underlier
  double vol_level{0.0};                  // additive ATM vol move
  double vol_skew{0.0};                   // additive skew move
  double rate{0.0};                       // additive rate move
  bool flat_smile{false};                 // vol independent of strike (pure-level tests)
  bool all_essvi{false};                  // build every underlier on eSSVI
};

// The standard shifted state of the suites: every axis moves.
[[nodiscard]] inline StateMove shifted_move() noexcept {
  StateMove m;
  m.time_units = kDayUnits;
  m.spot_ret = {0.008, -0.006, 0.005};
  m.vol_level = 0.012;
  m.vol_skew = 0.02;
  m.rate = 0.001;
  return m;
}

[[nodiscard]] inline PricedSurface make_surface(std::size_t ui, const StateMove &mv) {
  const UnderlierSpec &u = kUnderliers[ui];
  const double S = u.spot * (1.0 + mv.spot_ret[ui]);
  const double r = kRate + mv.rate;
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (std::size_t i = 0; i < kExpiryUnits.size(); ++i) {
    const std::int64_t units = kExpiryUnits[i] - mv.time_units;
    if (units <= 0) {
      continue; // expired at this instant: no pillar
    }
    const double T = years(units);
    const double F = S * std::exp((r - u.q) * T);
    const double df = std::exp(-r * T);
    const double vol = atm_vol(u, T) + mv.vol_level;
    if (u.convex && !mv.all_essvi) {
      constexpr int kNodes = 24;
      const double skew = mv.flat_smile ? 0.0 : u.skew + mv.vol_skew;
      ConvexSliceFit fit;
      fit.T = T;
      fit.F = F;
      fit.df = df;
      fit.rmse_price = 0.3;
      fit.n_obs = static_cast<std::size_t>(kNodes);
      fit.n_active = 4;
      fit.u.resize(static_cast<std::size_t>(kNodes));
      fit.C.resize(static_cast<std::size_t>(kNodes));
      for (int j = 0; j < kNodes; ++j) {
        const double K = F * (0.6 + 0.9 * static_cast<double>(j) / (kNodes - 1));
        const double sigma = vol + skew * std::log(K / F);
        fit.u[static_cast<std::size_t>(j)] = K;
        fit.C[static_cast<std::size_t>(j)] = black76_price(F, K, T, sigma, df, Side::Call);
      }
      cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    } else {
      EssviParams e{};
      e.theta = vol * vol * T;
      e.phi = mv.flat_smile ? 0.0 : 1.0 / std::sqrt(e.theta);
      e.rho = mv.flat_smile ? 0.0 : u.skew + mv.vol_skew;
      e.psi = 0.5;
      e.p = 0.5;
      e.lambda = 0.5;
      e.T = T;
      e.F = F;
      e.expiry_ns = expiry_ns(i);
      e.expiry_id = static_cast<std::uint16_t>(i);
      cs.push(std::make_unique<EssviCurve>(e, df));
    }
    ctx.push_back(SliceContext{T, F, 0.0, u.q, 200, 0});
  }
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = instant_ns(mv.time_units);
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = u.uid;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

// A state that owns its surfaces and storage (heap-pinned: the MarketState views
// point into both, so the aggregate never moves).
struct OwnedState {
  std::vector<PricedSurface> surfaces;
  MarketStateStorage storage;
  MarketState state;
};

// `carry == Surface`: built by market_state_from_surfaces (spot / rate / carry from
// the surfaces). `carry == State`: the same underliers with flat_q = the spec's q, so
// F = S exp((r - q) T) is computed by the state instead of read off the surface.
[[nodiscard]] inline std::unique_ptr<OwnedState>
make_state(const StateMove &mv, CarryFrom carry = CarryFrom::Surface, std::size_t n_uids = 3) {
  auto out = std::make_unique<OwnedState>();
  out->surfaces.reserve(n_uids);
  for (std::size_t ui = 0; ui < n_uids; ++ui) {
    out->surfaces.push_back(make_surface(ui, mv));
  }
  std::vector<SurfaceRef> refs;
  refs.reserve(n_uids);
  for (const PricedSurface &s : out->surfaces) {
    refs.emplace_back(&s);
  }
  out->storage = market_state_from_surfaces(instant_ns(mv.time_units), TimeSpec{}, refs).value();
  if (carry == CarryFrom::State) {
    for (std::size_t ui = 0; ui < out->storage.underliers.size(); ++ui) {
      out->storage.underliers[ui].carry = CarryFrom::State;
      out->storage.underliers[ui].flat_q = kUnderliers[ui].q;
    }
  }
  out->state = make_market_state(out->storage).value();
  return out;
}

// The 61-leg book: per underlier and expiry, five strikes around spot (puts below,
// calls above, the ATM strike alternating side by expiry), signed quantities, plus a
// repeat of the first contract so the dedup is exercised (60 unique contracts,
// 24 (uid, expiry, side) groups at n_uids = 3).
[[nodiscard]] inline std::vector<OptionLeg> make_option_legs(std::size_t n_uids = 3) {
  constexpr std::array<double, 5> kRel{0.92, 0.97, 1.0, 1.03, 1.08};
  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  for (std::size_t ui = 0; ui < n_uids; ++ui) {
    const UnderlierSpec &u = kUnderliers[ui];
    for (std::size_t ei = 0; ei < kExpiryUnits.size(); ++ei) {
      for (const double rel : kRel) {
        const double K = std::round(u.spot * rel * 2.0) / 2.0;
        const Side side = rel < 1.0 ? Side::Put
                          : rel > 1.0 ? Side::Call
                                      : (ei % 2u == 0u ? Side::Put : Side::Call);
        const double qty = static_cast<double>(id % 7u + 1u) * (id % 2u == 1u ? 1.0 : -1.0);
        legs.push_back(OptionLeg{id, u.uid, K, expiry_ns(ei), side, qty, 100.0});
        ++id;
      }
    }
  }
  OptionLeg repeat = legs.front();
  repeat.id = id;
  repeat.qty = 2.5;
  legs.push_back(repeat);
  return legs;
}

[[nodiscard]] inline std::vector<ShareLeg> make_share_legs() {
  return {ShareLeg{1001u, 1u, 150.0}, ShareLeg{1002u, 2u, -40.0}, ShareLeg{1003u, 1u, -60.0}};
}

// A dense book of ~n_target unique contracts (both sides at every strike between
// 0.85 S and 1.15 S) for the throughput smoke.
[[nodiscard]] inline std::vector<OptionLeg> make_dense_legs(std::size_t n_target) {
  const std::size_t per_slice = n_target / (kUnderliers.size() * kExpiryUnits.size());
  const std::size_t n_strikes = per_slice / 2u < 2u ? 2u : per_slice / 2u;
  std::vector<OptionLeg> legs;
  legs.reserve(n_strikes * 2u * kUnderliers.size() * kExpiryUnits.size());
  std::uint64_t id = 1;
  for (const UnderlierSpec &u : kUnderliers) {
    for (std::size_t ei = 0; ei < kExpiryUnits.size(); ++ei) {
      for (std::size_t s = 0; s < n_strikes; ++s) {
        const double rel = 0.85 + 0.30 * static_cast<double>(s) / static_cast<double>(n_strikes - 1u);
        const double K = u.spot * rel;
        for (const Side side : {Side::Call, Side::Put}) {
          legs.push_back(OptionLeg{id, u.uid, K, expiry_ns(ei), side, 1.0, 100.0});
          ++id;
        }
      }
    }
  }
  return legs;
}

// PortfolioPricer positions for the same option legs at the state `at` (T measured
// from it), for the oracle-parity test.
[[nodiscard]] inline std::vector<Position> to_positions(const std::vector<OptionLeg> &legs,
                                                        std::int64_t at_ns) {
  std::vector<Position> out;
  out.reserve(legs.size());
  for (const OptionLeg &l : legs) {
    const double T = static_cast<double>(l.expiry_ns - at_ns) / kCalendarYearNs;
    out.push_back(Position{l.id, OptionContract{l.uid, l.K, T, l.side}, l.qty, l.multiplier});
  }
  return out;
}

} // namespace atx::vol::risk_test
