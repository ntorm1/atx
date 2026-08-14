#pragma once

// CBOE-methodology discrete-strike variance replication (Task F-9, PV-F2 /
// LIT-1). Tier-B: additive, depends only on `types.hpp`.
//
// WHY THIS EXISTS AT ALL. Everywhere else this library prices variance off a
// FITTED surface -- an analytic smile quadratured on a synthetic log-strike
// grid (`var_swap_fair_strike`, derivatives.hpp). A LISTED variance future does
// not settle that way. It settles against the exchange's own finite sum over
// the strikes actually quoted at the settlement snapshot, using the exchange's
// own strike spacing, its own out-of-the-money selection, and its own zero-bid
// truncation. Those two numbers are not the same number, and the gap between
// them -- the BASIS -- is exactly what a desk hedging OTC variance with listed
// variance carries. Before this module there was no way to measure it (PV-F2).
//
// So this module computes the second number and nothing else. It reads a quote
// board and returns a variance strike; it holds no surface, fits nothing, and
// deliberately shares NO code with the parametric strip, so that a basis
// measured between the two is a measurement rather than a tautology.
//
// THE FORMULA, verbatim from the published methodology:
//
//   sigma^2 = (2/T) * SUM_i [ (dK_i / K_i^2) * (1/df) * Q(K_i) ]
//             - (1/T) * (F/K_0 - 1)^2
//
//   dK_i  midpoint strike spacing, (K_{i+1} - K_{i-1}) / 2, with the ENDPOINT
//         convention below;
//   K_0   the anchoring strike, below;
//   Q(K)  the MIDPOINT of the bid/ask spread of the out-of-the-money option at
//         K -- puts below K_0, calls above it, and the AVERAGE of the two mids
//         at K_0 itself, which is the one strike contributing both legs;
//   1/df  the published e^{RT} factor, expressed against the discount factor
//         this library already passes everywhere else rather than as a rate,
//         so a caller never has to re-derive R from a curve it already has.
//
// The trailing term is the Taylor remainder of ln(F/K_0) about K_0: the sum
// above replicates the log contract struck at K_0, not at F, and this closes
// the gap. It is exactly zero when F lands on a listed strike -- see `k0`.
//
// TWO READINGS THE PUBLISHED TEXT LEAVES OPEN, and what this module ships:
//
//  1. K_0. The published wording is "the first strike below the forward level
//     F", which does not say what happens when F IS a listed strike. This
//     module takes K_0 = the LARGEST LISTED STRIKE <= F. Reason: the trailing
//     term is a correction for the gap between F and K_0, so when there is no
//     gap the correction must vanish, and under the strict-inequality reading
//     a forward sitting exactly on a strike would carry a correction for a gap
//     that does not exist. The two readings differ only on the measure-zero set
//     F == K_listed (a forward is a computed quantity), so this costs nothing
//     and removes an artificial special case. `CboeVarStrip.taylor_term` is
//     exactly 0.0 in that case, which is testable and is tested.
//
//  2. WHICH neighbours dK_i uses. The published note ("dK for the lowest strike
//     is simply the difference between the lowest strike and the next higher
//     strike", and symmetrically at the top) is silent on whether "next" means
//     next ON THE BOARD or next IN THE SELECTED STRIP. This module uses the
//     SELECTED STRIP: a midpoint rule exists so the widths TILE the strike
//     axis, and after zero-bid exclusion has removed interior strikes only the
//     selected-strip neighbours still tile it without gaps. Using board
//     neighbours would leave the excluded strikes' width unrepresented, i.e.
//     would silently under-integrate exactly the regions the exclusion rule
//     touched.
//
// SELECTION AND TRUNCATION, verbatim:
//   - Walk DOWN from the strike below K_0 taking PUTS, and UP from the strike
//     above K_0 taking CALLS.
//   - Drop any option whose BID is zero (or non-positive): a no-bid quote is
//     not a price.
//   - Once TWO CONSECUTIVE listed strikes on a side both have a zero bid, stop:
//     nothing further out on that side is considered, even if it is quoted.
//     This is a hard stop, not a skip -- a lone quoted wing beyond a two-strike
//     no-bid gap is DISCARDED, which is the whole point of the rule (it refuses
//     to trust an isolated deep-wing print) and is what
//     `zero_bid_truncated_low`/`_high` report.
//   - K_0 itself is exempt from the zero-bid exclusion: the methodology selects
//     both K_0 options unconditionally. A K_0 with no bid on either leg
//     therefore contributes Q(K_0) computed from whatever is quoted rather than
//     dropping out; that is the published behaviour and, on a real board, the
//     at-the-money strike always has a bid.
//
// WHAT THIS MODULE IS NOT. It is a SINGLE-EXPIRY variance strike. The published
// index level additionally interpolates two expiries onto a fixed 30-day
// horizon and scales by 100; that is an index construction, not a settlement
// primitive, and it is not here.
//
// Thread-safety: every entry point is a stateless pure function of its
// arguments -- safe to call concurrently from any number of threads.
//
// Allocation: `cboe_var_strike` performs ONE heap allocation, for the per-term
// audit trail (`CboeVarStrip::terms`). This is a settlement / research entry,
// not a pricing hot path, and the audit trail is the reason the module is
// trustworthy: every published intermediate -- each dK_i, each Q(K_i), which
// leg supplied it -- is recoverable from the returned value rather than having
// to be re-derived by a reader who wants to check the sum.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/detail/aggregate_arity.hpp" // field-count drift pins, below
#include "atx/vol/types.hpp"

namespace atx::vol {

// One listed strike's two-sided board row: the call and the put at that strike.
//
// A ROW, not an option, because the methodology's selection rule is stated per
// strike and needs BOTH legs available at K_0 to average them. A caller holding
// per-option quotes collapses them into rows before calling; a caller whose
// board genuinely has no call (or no put) at some strike leaves that leg at
// zero, which reads as "no bid" and is excluded on the side that would have
// used it -- the same treatment a quoted-but-bidless option gets.
//
// Bid/ask are the RAW quotes. This module takes the midpoint itself, so that
// the mid convention is stated in exactly one place.
struct CboeStrikeQuote {
  double strike = 0.0;
  double call_bid = 0.0;
  double call_ask = 0.0;
  double put_bid = 0.0;
  double put_ask = 0.0;
};

// Which leg of the board a selected strike's Q(K) came from. `K0Average` is the
// single strike that contributes the average of both mids.
enum class CboeStripLeg : std::uint8_t {
  Put = 0,
  Call = 1,
  K0Average = 2,
};

// One term of the sum, retained so a reader can audit the published
// intermediates rather than re-deriving them.
struct CboeStripTerm {
  double strike = 0.0;  // K_i
  double delta_k = 0.0; // dK_i, resolved under the selected-strip reading above
  double mid = 0.0;     // Q(K_i)
  // (dK_i / K_i^2) * (1/df) * Q(K_i) -- the summand BEFORE the leading 2/T.
  // `sum_term` is (2/T) times the sum of these, computed from these, so the
  // identity holds to the last bit and is not merely approximate.
  double contribution = 0.0;
  CboeStripLeg leg = CboeStripLeg::Put;
};

// The resolved strike plus every intermediate the methodology names.
struct CboeVarStrip {
  // The answer: annualized decimal variance, sigma^2. Same units as
  // `DerivQuote::var_strike_dec` -- 0.04 <-> 20 vol -- so the two are directly
  // differenceable, which is the entire point of `cboe_parametric_basis`.
  double var_strike_dec = 0.0;
  double vol_strike_dec = 0.0; // sqrt(var_strike_dec), decimal vol

  double k0 = 0.0;            // the anchoring strike actually used
  double sum_term = 0.0;      // (2/T) * SUM contributions
  double taylor_term = 0.0;   // -(1/T) * (F/K_0 - 1)^2, so var == sum + taylor

  std::size_t n_puts = 0;  // selected strikes strictly below k0
  std::size_t n_calls = 0; // selected strikes strictly above k0

  // TRUE when the walk on that side stopped on the two-consecutive-zero-bid
  // rule, i.e. the board had more strikes out there and the rule refused them.
  // FALSE when the walk simply ran off the end of the board. The distinction
  // matters: the first says the strip is truncated by quote QUALITY (wing
  // information exists and was rejected), the second says it is truncated by
  // listing COVERAGE (there is nothing further out). Only the first is a
  // statement about the snapshot.
  bool zero_bid_truncated_low = false;
  bool zero_bid_truncated_high = false;

  double k_lo = 0.0; // lowest selected strike
  double k_hi = 0.0; // highest selected strike

  std::vector<CboeStripTerm> terms; // ascending in strike; size == n_puts+n_calls+1
};

// Drift pins, following the `DerivConfig`/`DerivQuote`/`RealizedVarianceSpec`
// convention: every construction site names its fields, so a future append
// raises no migration burden -- these exist only to make that append VISIBLE
// rather than silent. Stated at birth rather than retrofitted, because the
// convention is only worth anything if a new aggregate joins it before it has
// any callers to break.
static_assert(detail::aggregate_arity_is_v<CboeStrikeQuote, 5>,
              "CboeStrikeQuote field count changed: update this pin.");
static_assert(detail::aggregate_arity_is_v<CboeStripTerm, 5>,
              "CboeStripTerm field count changed: update this pin.");
static_assert(detail::aggregate_arity_is_v<CboeVarStrip, 12>,
              "CboeVarStrip field count changed: update this pin.");

// The CBOE discrete-strike variance strike for one expiry.
//
// @param board  listed strikes, STRICTLY ASCENDING in `strike`, one row per
//               strike. Borrowed for the duration of the call only.
// @param forward   F, the forward index level for this expiry (> 0).
// @param df        the discount factor to this expiry, i.e. e^{-RT} (> 0). The
//                  published e^{RT} factor is 1/df.
// @param maturity_t T in years (> 0).
//
// @return InvalidArgument when: `forward`, `df` or `maturity_t` is non-finite
//         or non-positive; the board has fewer than two rows; a strike is
//         non-finite, non-positive, or not strictly greater than its
//         predecessor; any quote field is non-finite or negative; any ask is
//         below its own bid; or fewer than two strikes survive selection (a
//         board where every wing is bidless leaves only K_0, and a single
//         strike has no dK -- this is the "all zero bid" degenerate case, and
//         it returns a Status rather than a NaN strike).
//         OutOfRange when `forward` is below the lowest listed strike (no K_0
//         exists), or when the resolved variance is negative -- which a real
//         board cannot produce, and which would otherwise hand the caller a NaN
//         `vol_strike_dec`.
[[nodiscard]] Result<CboeVarStrip> cboe_var_strike(std::span<const CboeStrikeQuote> board,
                                                   double forward, double df, double maturity_t);

// The basis diagnostic (PV-F2): the same expiry's LISTED settlement variance
// against a PARAMETRIC variance strike, on the same board and the same clock.
//
// `parametric_var_dec` is passed IN rather than computed here, deliberately.
// The parametric side is a template over surface type (`var_swap_fair_strike`,
// derivatives.hpp) and carries a whole quality/wing/corridor policy with it;
// binding one particular instantiation into this module would (a) make the
// basis a statement about that policy rather than about the board, and (b)
// couple the listed-settlement primitive to the fitting stack it exists to be
// independently comparable against. The caller states which parametric number
// it wants compared, and the comparison stays honest.
//
// @return everything `cboe_var_strike` does, plus InvalidArgument when
//         `parametric_var_dec` is non-finite or negative.
struct CboeParametricBasis {
  double cboe_var_dec = 0.0;       // the listed sum
  double parametric_var_dec = 0.0; // as supplied by the caller
  double basis_var_dec = 0.0;      // cboe - parametric, in decimal VARIANCE
  // cboe - parametric in decimal VOL, i.e. sqrt of each. Reported alongside the
  // variance basis because a desk sizes the hedge in vega, and a variance gap
  // that looks tiny at 4e-4 is 1 vol point at a 20-vol level and 0.4 at a
  // 50-vol one -- the same number means different things at different levels,
  // so both are published rather than leaving the reader to convert.
  double basis_vol_dec = 0.0;
  CboeVarStrip strip{}; // the listed side's full audit trail
};

static_assert(detail::aggregate_arity_is_v<CboeParametricBasis, 5>,
              "CboeParametricBasis field count changed: update this pin.");

[[nodiscard]] Result<CboeParametricBasis>
cboe_parametric_basis(std::span<const CboeStrikeQuote> board, double forward, double df,
                      double maturity_t, double parametric_var_dec);

} // namespace atx::vol
