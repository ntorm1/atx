#pragma once

// MarketEnv — the market-environment aggregate a self-contained PricerFitter
// takes instead of loose (spot, rate, timestamp) scalars.
//
// The atx-vol fit path historically threaded spot / rate / valuation-time as
// separate scalars through `OptionChain` and `SessionInputs`, and the rate was a
// single flat `double` end-to-end (the `YieldCurve` / `CurveSet` machinery existed
// but was dead relative to the fitter). `MarketEnv` bundles the whole environment
// into one value the caller fills once:
//
//   * `spot`       — the pricing spot (an OPRA panel's PCP-implied spot, say).
//   * `now_ns`     — the valuation timestamp (epoch ns).
//   * a RATE CURVE — either a flat continuously-compounded `flat_rate`, or a term
//                    `YieldCurve` (Fritsch-Carlson monotone cubic-Hermite). Query
//                    it at a maturity with `rate_at(T)`.
//   * `cash_divs`  — the discrete cash-dividend schedule for the hybrid forward.
//
// ## Rate-curve usage (documented scope)
//
// `rate_at(T)` returns the term rate: the yield curve's zero rate at T when the
// curve carries pillars, else `flat_rate`. `PricerFitter` lowers the env to the
// fit pipeline using the rate at a representative maturity (the front listed
// expiry). The dominant per-expiry carry is recovered from the market itself via
// the de-Americanization borrow solve (the effective carry q_eff that reproduces
// each term forward), which absorbs the term structure the market is actually
// pricing; the rate mainly sets the discount and the American early-exercise
// premium, where a representative continuously-compounded rate is standard. Full
// per-expiry rate-curve integration into the carry is a documented enhancement.
//
// Value type: copyable / movable via its RAII members (Rule of Zero).

#include <cstdint>
#include <vector>

#include "atx/vol/rates_curve.hpp"  // YieldCurve, DividendEvent

namespace atx::vol {

struct MarketEnv {
  double spot{0.0};              // pricing spot (> 0)
  std::int64_t now_ns{0};        // valuation timestamp (epoch ns)
  double flat_rate{0.0};         // fallback continuously-compounded rate
  YieldCurve yield{};            // optional term rate curve; empty => flat_rate
  std::vector<DividendEvent> cash_divs{};  // discrete cash-dividend schedule

  // Continuously-compounded zero rate at year-fraction T: the yield curve when it
  // carries pillars, else the flat rate. `T <= 0` returns the flat rate (the
  // yield curve has no rate "at" the value date).
  [[nodiscard]] double rate_at(double T) const noexcept {
    if (yield.size() > 0 && T > 0.0) {
      return yield.zero(T);
    }
    return flat_rate;
  }

  // Convenience: the flat-rate environment (the common case; bit-identical to the
  // historical scalar path).
  [[nodiscard]] static MarketEnv flat(double spot, double r, std::int64_t now_ns,
                                      std::vector<DividendEvent> divs = {}) {
    MarketEnv env;
    env.spot = spot;
    env.now_ns = now_ns;
    env.flat_rate = r;
    env.cash_divs = std::move(divs);
    return env;
  }
};

}  // namespace atx::vol
