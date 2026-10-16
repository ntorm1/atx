#pragma once

// Carry-resolution policy for the American quote slice (quote_slice.hpp).
//
// `Supplied` is the historical path: the market reference yield, any
// per-contract yield overrides and the configured dividend model are used as
// given. `ImpliedForward` replaces that carry, per expiry, with the forward the
// slice's OWN raw put-call quotes imply (marketdata/implied_forward.hpp):
//
//   1. pair calls and puts at each strike (bidless legs are not point quotes);
//   2. estimate the forward F on the slice's pricer clock (variance years, with
//      the funding rate rescaled so the discount factor is preserved);
//   3. restate F as the market's calendar continuous yield UNDER the configured
//      dividend model, keeping the supplied aggregate dividend PV:
//        ContinuousYield              q = r - ln(F / S) / T_cal
//        AggregatePresentValue        q = r - ln(F / (S - PV)) / T_cal
//        AggregateFundingPresentValue q = -ln((F e^{-r T_cal} + PV) / S) / T_cal
//      so the slice's own forward and effective American carry reproduce F;
//   4. drop every per-contract yield override (the implied carry is one per
//      expiry), then fit exactly as the Supplied path does.
//
// A refusal (sparse expiry, dispersion, early-exercise contamination, or the
// DatedCash model, whose lattice forward the continuous-carry estimator cannot
// represent) falls back to the Supplied carry; the fallback fit is bit-identical
// to a Supplied fit of the same inputs. The resolution is always recorded.

#include <cstddef>
#include <cstdint>
#include <limits>

#include "atx/vol/api/marketdata/implied_forward.hpp"

namespace atx::vol {

enum class QuoteSliceCarryMode : std::uint8_t {
  Supplied,      // market/contract yields as given (default; historical behaviour)
  ImpliedForward // per-expiry forward implied by the slice's raw put-call quotes
};

[[nodiscard]] const char *to_string(QuoteSliceCarryMode value) noexcept;

struct QuoteSliceCarryConfig {
  QuoteSliceCarryMode mode = QuoteSliceCarryMode::Supplied;
  ImpliedForwardConfig implied{};
};

// What the carry step decided for one expiry. Default-constructed (mode
// Supplied, applied false) on the historical path.
struct QuoteSliceCarryResolution {
  QuoteSliceCarryMode mode = QuoteSliceCarryMode::Supplied;
  bool applied = false; // the implied carry replaced the supplied carry
  ImpliedForwardStatus status = ImpliedForwardStatus::Invalid;
  const char *reason = "supplied carry";
  // The supplied carry, as the slice would have used it.
  double supplied_forward = std::numeric_limits<double>::quiet_NaN();
  double supplied_yield = std::numeric_limits<double>::quiet_NaN(); // calendar
  // The implied carry: forward, calendar yield under the dividend model, and
  // the total continuous carry on the pricer clock (dividends + borrow).
  double implied_forward = std::numeric_limits<double>::quiet_NaN();
  double implied_yield = std::numeric_limits<double>::quiet_NaN(); // calendar
  double implied_total_yield = std::numeric_limits<double>::quiet_NaN(); // pricer clock
  double parity_forward = std::numeric_limits<double>::quiet_NaN();
  double forward_lower = std::numeric_limits<double>::quiet_NaN();
  double forward_upper = std::numeric_limits<double>::quiet_NaN();
  double dispersion_sd = std::numeric_limits<double>::quiet_NaN();
  double exercise_shift_sd = std::numeric_limits<double>::quiet_NaN();
  double atm_total_vol = std::numeric_limits<double>::quiet_NaN();
  std::size_t pairs_valid = 0;
  std::size_t pairs_selected = 0;
  std::size_t pairs_retained = 0;
  std::size_t pairs_contaminated = 0;
  std::size_t pairs_unidentified = 0;
};

} // namespace atx::vol
