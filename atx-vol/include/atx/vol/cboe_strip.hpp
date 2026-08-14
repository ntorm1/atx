#pragma once

// Cboe-methodology discrete-strike variance replication (Task F-9, PV-F2 /
// LIT-1). Tier-B: additive, depends only on `types.hpp`.
//
// WHY THIS EXISTS AT ALL. Everywhere else this library prices variance off a
// FITTED surface -- an analytic smile quadratured on a synthetic log-strike
// grid (`var_swap_fair_strike`, derivatives.hpp). A LISTED variance future does
// not settle that way. It settles against the exchange's own finite sum over
// the strikes actually quoted at the settlement snapshot, using the exchange's
// own strike spacing, its own out-of-the-money selection, and its own quote
// exclusion rules. Those two numbers are not the same number, and the gap
// between them -- the BASIS -- is exactly what a desk hedging OTC variance with
// listed variance carries. Before this module there was no way to measure it
// (PV-F2).
//
// So this module computes the second number and nothing else. It reads a quote
// board and returns a variance strike; it holds no surface, fits nothing, and
// deliberately shares NO code with the parametric strip, so that a basis
// measured between the two is a measurement rather than a tautology.
//
// ── GOVERNING SOURCE ────────────────────────────────────────────────────────
//
// Every rule below is implemented from, and cited against, the CURRENT Cboe
// text -- not the retired white papers:
//
//   [CUR-M]  Cboe Global Indices, "Cboe Volatility Index -- Mathematics
//            Methodology", Version 5.0, revised 2026-02-26.
//            https://cdn.cboe.com/api/global/us_indices/governance/
//              Cboe_Volatility_Index_Mathematics_Methodology.pdf
//   [CUR-V]  Cboe Global Indices, "Cboe Volatility Index -- Methodology",
//            Version 6.0, revised 2026-02-26 (carries the worked example in
//            Appendices 3-5).
//            https://cdn.cboe.com/api/global/us_indices/governance/
//              Volatility_Index_Methodology_Cboe_Volatility_Index.pdf
//
// `cboe_strip_test.cpp` reproduces [CUR-V]'s published 268-row worked example
// end to end, which is what pins the rules below to the source rather than to
// this comment.
//
// THE FORMULA, [CUR-M] §3(a)(iv):
//
//   sigma^2 = (2/T) * SUM_i [ (dK_i / K_i^2) * (1/df) * Q(K_i) ]
//             - (1/T) * (F/K_0 - 1)^2
//
//   dK_i  strike spacing, below;
//   K_0   the anchoring strike, below;
//   Q(K)  the MIDPOINT of the bid/ask spread of the out-of-the-money option at
//         K -- puts below K_0, calls above it, and the AVERAGE of the two mids
//         at K_0 itself, which is the one strike contributing both legs;
//   1/df  the published e^{RT} factor, expressed against the discount factor
//         this library already passes everywhere else rather than as a rate,
//         so a caller never has to re-derive R from a curve it already has.
//
// The trailing term is the Taylor remainder of ln(F/K_0) about K_0: the sum
// replicates the log contract struck at K_0, not at F, and this closes the gap.
//
// ── K_0: "equal to or otherwise immediately below F" ────────────────────────
//
// [CUR-M] §3(a) symbol table, and again in §3(a)(ii): "K_0  First strike EQUAL
// TO OR OTHERWISE IMMEDIATELY BELOW F". [CUR-V] Appendix 3 repeats it. So
// `K_0 = max{K : K <= F}` is the published rule, implemented here verbatim; it
// is not a house reading, and the inclusive tie is not an implementation detail
// free to be "tidied" into a strict `<`.
//
// THE HISTORY IS WORTH KEEPING, because it is why this looked disputable: the
// 2009 edition said "First strike BELOW the forward index level, F", and the
// 2019 edition is internally INCONSISTENT -- its symbol table still carries the
// 2009 strict wording while its body text carries the current inclusive one.
// Both quotations are real, which is how two careful readers reach opposite
// conclusions from "the white paper". The current text resolves it inclusively
// in both places, and the current text governs.
//
// A consequence, not a justification: when F lands exactly on a listed strike
// the trailing term is exactly zero, because there is no F-to-K_0 gap left to
// correct. The tie is NOT numerically negligible -- F is computed at the strike
// MINIMISING |C - P| ([CUR-M] §3(a)(ii)), so a forward landing on a listed
// strike is actively selected for rather than a null event, and on this
// module's own test fixture the two readings differ by 5 vol points.
//
// ── dK: from the SELECTED strip, not from the board ─────────────────────────
//
// [CUR-M] §3(a) symbol table: "Highest OTM Strike K_i: K_i - K_{i-1} · Lowest
// OTM Strike K_i: K_{i+1} - K_i · Otherwise: (K_{i+1} - K_{i-1})/2", and
// §3(a)(iv): "Determine dK for each strike INCLUDED IN THE CALCULATION ... at
// the upper and lower edges of any given SET OF OPTIONS". [CUR-V] Appendix 3 is
// explicit that the set is the strip: "the 1370 Put is the lowest strike IN THE
// STRIP of near-term options".
//
// The published worked example settles it numerically as well as textually, and
// that is the part worth trusting: it contains six strikes whose BOARD
// neighbour differs from their STRIP neighbour, because an interior strike was
// excluded for quote quality (near-term 1400 / 1410 / 1420 puts and the 2100
// call; next-term 1275 / 1325 puts). Re-deriving all 268 published dK values
// from the SURVIVING strikes reproduces every one; re-deriving them from the
// BOARD fails at exactly those six. `WhitePaper_*` in the test file asserts
// them individually.
//
// ── Selection and exclusion, [CUR-M] §3(a)(iii) ─────────────────────────────
//
//   - Walk DOWN from the strike below K_0 taking PUTS, and UP from the strike
//     above K_0 taking CALLS.
//   - "Exclude any put option that has a bid price OR ASK PRICE equal to zero."
//     The zero-ASK half is a rule change effective 2025-02-10 ([CUR-M]
//     Appendix 1); before it, only a zero bid excluded. Note what this means for
//     a bid-with-no-offer series -- a routine deep-wing state: it is EXCLUDED,
//     not treated as a malformed quote, and the rest of the board still
//     computes.
//   - "Once two [options] with consecutive strike prices are found to have zero
//     bid prices or zero ask prices, EXCLUDE THE OBSERVED option(s) and consider
//     no [options] with lower/higher strikes for inclusion." So the two
//     triggering strikes are themselves out, and this is a hard stop rather than
//     a skip: a quoted wing beyond a two-strike exclusion gap is DISCARDED.
//     [CUR-V] Appendix 3 makes that explicit ("Note that the 1350 and 1355 put
//     options are not included despite having non-zero bid prices"), and the
//     test asserts exactly those strikes.
//   - K_0 itself is exempt from the WALK: "Finally, select both the put and call
//     options with strike price K_0." It is NOT exempt from being unquotable --
//     see the non-calculable conditions below.
//
// ── WHEN THE INDEX CANNOT BE CALCULATED ─────────────────────────────────────
//
// [CUR-M] §5(b) enumerates two conditions under which there IS no answer, and
// this module returns `ErrorCode::Unavailable` for both rather than a number:
//
//   1. §5(b)(1), from §3(a)(ii): "If quotes of the K_0 put option or the K_0
//      call option are NULL or the bid price is higher than the ask price, then
//      the Cboe volatility index cannot be calculated." BOTH sub-clauses are
//      implemented, and the second is why the two ORIENTATIONS of a one-sided
//      K_0 quote are treated differently rather than collapsed together:
//        0.00/0.00 -- null. Refused.
//        0.30/0.00 -- a bid literally above its ask. The clause names it.
//                     Refused.
//        0.00/0.30 -- no bid but a REAL offer. Not null, bid not above ask;
//                     served, and its midpoint of 0.15 is a genuine price.
//      A K_0 leg crossed with BOTH sides quoted (0.30/0.20) never reaches this
//      test: it is malformed input, rejected board-wide as `InvalidArgument`.
//      See `cboe_var_strike`'s `@return` for that split, which is tested both
//      ways.
//   2. §5(b)(2): "if all out-of-the-money call options have been excluded OR all
//      out-of-the-money put options have been excluded, then the volatility
//      index spot value cannot be calculated." A one-sided strip is not a
//      cheaper answer; it is a different claim -- half the log contract.
//
// `Unavailable` and not `InvalidArgument`, deliberately: the board is
// well-formed and the caller did nothing wrong. This is the SNAPSHOT saying it
// does not admit an index -- a condition the exchange itself handles by
// republishing the last valid value. A caller that cannot tell the two apart
// cannot implement that behaviour.
//
// ONE QUESTION THE SOURCE LEAVES OPEN, recorded rather than answered, and it is
// narrower than it first looks. §3(a)(ii) decides the null and bid-above-ask K_0
// cases outright, so the ONLY undecided case is the remaining orientation:
//
//   does a K_0 leg quoted 0.00/0.30 -- no bid, a real offer -- make the index
//   non-calculable?
//
// §3(a)(iii)'s zero-bid-or-zero-ask exclusion would exclude it, but that
// exclusion sits inside clauses scoped to K < K_0 and K > K_0, while K_0 is
// picked up by an unconditional "Finally, select both the put and call options
// with strike price K_0". So the walk's rule does not reach it and no other text
// addresses it.
//
// This module takes the NARROW reading -- it is served, contributing its genuine
// midpoint -- because that is what the text actually says, and the broad reading
// would be this module inventing a rule and then testing itself against it. A
// reader who can put the question to Cboe should; if the answer is the broad
// reading, `check_k0_quotable` (cboe_strip.cpp) is the one function to change and
// `NotCalculable_ZeroBidK0LegWithARealOfferIsServed` is the test that records
// today's choice. Nothing else above is in question -- an earlier revision of
// this paragraph named both orientations together, which merged a decided case
// with an open one and is withdrawn.
//
// ── WHAT THIS MODULE IS NOT ─────────────────────────────────────────────────
//
// A SINGLE-EXPIRY variance strike. The published index additionally interpolates
// two expiries onto a fixed 30-day horizon and scales by 100 ([CUR-V] Appendix
// 3); that is an index construction, not a settlement primitive, and it is not
// here. The test performs that composition itself, closing the published example
// out at VIX = 13.93 -- the honest way to show both strikes are right without
// pretending this module computes an index.
//
// Thread-safety: every entry point is a stateless pure function of its
// arguments -- safe to call concurrently from any number of threads.
//
// Allocation: `cboe_var_strike` builds its per-term audit trail
// (`CboeVarStrip::terms`) on the heap. No allocation COUNT is claimed here -- a
// count in a comment is an unverified assertion that rots, and this is a
// settlement / research entry rather than a pricing hot path, so the number
// would be load-bearing for nobody. The audit trail earns its keep by making
// every published intermediate -- each dK_i, each Q(K_i), which leg supplied it
// -- recoverable from the returned value instead of having to be re-derived by
// a reader checking the sum.

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
// per-option quotes collapses them into rows before calling.
//
// A leg left at 0.00/0.00 reads as a NULL quote -- no market. Away from K_0 that
// simply excludes it on the side that would have used it; AT K_0 it makes the
// index non-calculable ([CUR-M] §3(a)(ii)). Bid/ask are the RAW quotes: this
// module takes the midpoint itself, so the mid convention is stated in exactly
// one place.
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
  double delta_k = 0.0; // dK_i, resolved over the SELECTED strip (see above)
  double mid = 0.0;     // Q(K_i)
  // (dK_i / K_i^2) * (1/df) * Q(K_i) -- the summand BEFORE the leading 2/T.
  // This is exactly the "Individual Contribution" column [CUR-V] Appendix 5
  // publishes per strike. `sum_term` is (2/T) times the sum of these, computed
  // from these, so the identity holds to the last bit rather than approximately.
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

  double k0 = 0.0;          // the anchoring strike actually used
  double sum_term = 0.0;    // (2/T) * SUM contributions
  double taylor_term = 0.0; // -(1/T) * (F/K_0 - 1)^2, so var == sum + taylor

  std::size_t n_puts = 0;  // selected strikes strictly below k0
  std::size_t n_calls = 0; // selected strikes strictly above k0

  // TRUE when the two-consecutive-zero-quote rule stopped the walk AND the board
  // still carried at least one further listed strike beyond the stop -- i.e. the
  // rule actually REFUSED listed strikes. FALSE when the walk ran off the end of
  // the board, INCLUDING the case where the triggering pair itself is that end
  // and nothing was refused.
  //
  // That last clause is the whole reason the flag is defined this way rather
  // than "the run counter reached two": boards whose outermost strikes are
  // bidless are the common case on an illiquid wing, so a flag that fired there
  // would read TRUE constantly and mean nothing. As defined, TRUE is a statement
  // about QUOTE QUALITY (wing information existed and the rule declined it),
  // while a short strip with the flag FALSE is a statement about LISTING
  // COVERAGE (there was nothing further out). A monitoring caller wants those
  // separated.
  bool zero_quote_truncated_low = false;
  bool zero_quote_truncated_high = false;

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

// The Cboe discrete-strike variance strike for one expiry.
//
// @param board  listed strikes, STRICTLY ASCENDING in `strike`, one row per
//               strike. Borrowed for the duration of the call only.
// @param forward    F, the forward index level for this expiry (> 0).
// @param df         the discount factor to this expiry, i.e. e^{-RT} (> 0). The
//                   published e^{RT} factor is 1/df.
// @param maturity_t T in years (> 0). [CUR-M] §3(a)(i) measures T in minutes /
//                   525,600; converting to years is the caller's job, and the
//                   test does it from the published minute counts.
// @param diagnostic_out  when non-null, ASSIGNED ON EVERY RETURN PATH --
//               success and every failure -- so the caller who most needs the
//               audit trail (the one whose board was refused) can still read how
//               far resolution got. Paths that failed before resolving anything
//               leave it default-constructed. `nullptr` (the default) simply
//               forgoes it. Same channel convention as
//               `forward_var_fair_strike`'s own `diagnostic_out`.
//
// @return InvalidArgument when the INPUT is malformed: `forward`, `df` or
//         `maturity_t` non-finite or non-positive; fewer than two board rows; a
//         strike non-finite, non-positive, or not strictly greater than its
//         predecessor; any quote field non-finite or negative; or a genuinely
//         crossed quote (ask < bid with BOTH non-zero -- note that bid > 0 with
//         ask == 0 is a legitimate one-sided market AWAY FROM K_0, not a crossed
//         quote, and is handled by the exclusion rule instead). This applies at
//         every strike INCLUDING K_0, and it fires before K_0 is resolved.
//         Unavailable when the board is well-formed but admits no index, per
//         [CUR-M] §5(b): a K_0 leg that is null (0.00/0.00) or carries a bid
//         above its ask with no offer (0.30/0.00), or an empty out-of-the-money
//         wing. The split against the line above is deliberate and tested both
//         ways: a K_0 leg crossed with BOTH sides quoted is malformed INPUT,
//         while a K_0 leg with no offer at all is a non-calculable SNAPSHOT.
//         See the header's non-calculable section.
//         OutOfRange when `forward` is below the lowest listed strike (no K_0
//         exists), or when the resolved variance is negative -- which a real
//         board cannot produce, and which would otherwise hand the caller a NaN
//         `vol_strike_dec`.
[[nodiscard]] Result<CboeVarStrip> cboe_var_strike(std::span<const CboeStrikeQuote> board,
                                                   double forward, double df, double maturity_t,
                                                   CboeVarStrip *diagnostic_out = nullptr);

// The basis diagnostic (PV-F2): the same expiry's LISTED settlement variance
// against a PARAMETRIC variance strike, on the same board and the same clock.
//
// `parametric_var_dec` is passed IN rather than computed here, deliberately. The
// parametric side is a template over surface type (`var_swap_fair_strike`,
// derivatives.hpp) and carries a whole quality/wing/corridor policy with it;
// binding one particular instantiation into this module would (a) make the basis
// a statement about that policy rather than about the board, and (b) couple the
// listed-settlement primitive to the fitting stack it exists to be independently
// comparable against. The caller states which parametric number it wants
// compared, and the comparison stays honest.
struct CboeParametricBasis {
  double cboe_var_dec = 0.0;       // the listed sum
  double parametric_var_dec = 0.0; // as supplied by the caller
  double basis_var_dec = 0.0;      // cboe - parametric, in decimal VARIANCE
  // cboe - parametric in decimal VOL, i.e. sqrt of each. Reported alongside the
  // variance basis because a desk sizes the hedge in vega, and a variance gap
  // that looks tiny at 4e-4 is 1 vol point at a 20-vol level and 0.4 at a 50-vol
  // one -- the same number means different things at different levels, so both
  // are published rather than leaving the reader to convert.
  double basis_vol_dec = 0.0;
  CboeVarStrip strip{}; // the listed side's full audit trail
};

static_assert(detail::aggregate_arity_is_v<CboeParametricBasis, 5>,
              "CboeParametricBasis field count changed: update this pin.");

// @return everything `cboe_var_strike` does, plus InvalidArgument when
//         `parametric_var_dec` is non-finite or negative. `diagnostic_out`
//         carries the same every-return-path guarantee.
[[nodiscard]] Result<CboeParametricBasis>
cboe_parametric_basis(std::span<const CboeStrikeQuote> board, double forward, double df,
                      double maturity_t, double parametric_var_dec,
                      CboeVarStrip *diagnostic_out = nullptr);

} // namespace atx::vol
