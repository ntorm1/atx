#pragma once

// Davis-Hobson quote feasibility: does ANY arbitrage-free model reproduce this
// expiry's call quotes?
//
// Reference: M.H.A. Davis & D.G. Hobson, "The Range of Traded Option Prices",
// Mathematical Finance 17(1):1-14 (2007), Theorem 3.1.
//
// This is a MODEL-INDEPENDENT PREDICATE, not a fit. It answers a question no
// calibrator can answer about itself: when a slice refuses to fit, is that our
// fitter's limitation or data that no fitter could ever serve? Nothing here
// knows about SVI, eSSVI, or any parametric family.
//
// ## COORDINATE WARNING -- `x` IS NOT LOG-MONEYNESS
//
// Everywhere else in atx-vol, `k` means log-moneyness ln(K/F). This file uses
// `x_i = K_i / F`, the NORMALISED STRIKE -- a positive ratio, 1.0 at the money,
// never negative. It is deliberately spelled `x` and never `k` so a reader who
// carries the log-moneyness reflex in from vol_curve.hpp / arb.hpp cannot
// silently misread the geometry. The companion price coordinate is
// `r_i = p_i / (DF * F)`, the call price as a fraction of the discounted
// forward, so that the adjoined point at zero strike is exactly (0, 1).
//
// (The sprint plan writes the price normalisation as `r_i = p_i / DF`. That is
// inconsistent with adjoining (0, 1) and with the stated bound R'(0+) >= -1:
// an undiscounted zero-strike call is worth DF*F, so p/DF lands the adjoined
// point at (0, F) and rescales the slope bound to -F. Dividing by DF*F is the
// normalisation the theorem is stated in, and is what this file implements.)
//
// ## The test
//
// Sort the quotes by strike, map them to (x_i, r_i), and adjoin (0, 1). Let R
// be the largest DECREASING CONVEX MINORANT of that point set -- the support
// function. Let n0 be the first index whose minorant value is (effectively)
// zero, or n if the prices never reach zero. Then the quotes are consistent
// with absence of arbitrage IFF
//
//   (1) R is strictly decreasing on [0, x_{n0 ^ n}],
//   (2) R'(0+) >= -1,
//   (3) R(x_i) = r_i for every quoted i.
//
// (1) and (3) are the call-spread and butterfly geometry; (2) is the intrinsic
// bound r_1 >= 1 - x_1. Violations are typed per the sprint's taxonomy:
// condition 3 alone is reported as WeakArbitrage (a quote sits strictly above
// its own convex minorant), conditions 1 and 2 as ModelIndependentArbitrage
// (a riskless profit no model can price away). When both fail the stronger
// verdict wins, so a negative call spread -- which flattens the minorant AND
// lifts the point off it -- reports ModelIndependentArbitrage.
//
// ## Cost and allocation
//
// Two O(n) passes over strike-sorted quotes: one monotone convex-hull build,
// one minorant evaluation. Zero dynamic allocation -- the hull stack is a
// caller-supplied scratch span. `noexcept`, pure, thread-safe (reads only its
// arguments). Cheap enough to sit in front of every slice fit.

#include <cstddef>
#include <cstdint>
#include <span>

namespace atx::vol::detail {

// One call quote on ONE expiry. Plain data, trivially copyable, no container.
//
// `price` is the undiscounted call price in currency units -- whichever
// representative the caller trusts (mid, bid, ask, or a de-Americanised
// equivalent). The side flags say which sides of the market backed it; a quote
// with NEITHER side present is not a quote and is skipped. They are carried
// rather than acted on beyond that screen because the bid-ask-box refinement
// of this test (Gerhold-Gulum) is a different theorem, not a tolerance on this
// one.
struct CallQuote {
  double strike{0.0}; // K > 0, in the same currency units as `price`
  double price{0.0};
  bool has_bid{false};
  bool has_ask{false};
};

// Per-expiry normalisation and the two tolerances the predicate needs.
struct FeasibilityInputs {
  // Forward price of the underlying to this expiry. Must be finite and > 0.
  double forward{0.0};
  // Discount factor to this expiry. Must be finite and > 0.
  double discount{1.0};

  // Absolute slack in NORMALISED price units (price / (DF * forward)), applied
  // to ALL THREE conditions. Condition 2 is toleranced on its price form
  // r_1 >= 1 - x_1 rather than on the slope, so the slack does not silently
  // shrink by a factor of x_1 at deep-in-the-money strikes.
  //
  // The default is a pure-arithmetic guard only; it does not model quote
  // quantisation or bid-ask width. On real mids a slice's own median
  // half-spread is the defensible setting -- a violation smaller than the
  // quotes' own precision is not a tradeable one.
  double price_tol{1e-12};

  // Normalised price at or below which a call counts as WORTHLESS, which is
  // what fixes n0 and therefore where condition 1 stops demanding strictness.
  //
  // The default 0.0 is the textbook statement and is WRONG FOR RAW MARKET
  // DATA: a run of deep-wing strikes all quoting 0.00 x 0.05 has an identical
  // mid, i.e. a flat minorant segment, i.e. condition 1 fails, on essentially
  // every board. A caller feeding real quotes must set this to roughly half a
  // minimum tick expressed in normalised units, e.g. 0.005 / (DF * forward).
  double zero_price_tol{0.0};
};

enum class FeasibilityVerdict : std::uint8_t {
  // Some arbitrage-free model reproduces these quotes exactly.
  Feasible = 0,
  // Condition 3 fails: a quote lies strictly above its own convex minorant.
  WeakArbitrage = 1,
  // Condition 1 or 2 fails, or the quotes contradict each other outright.
  ModelIndependentArbitrage = 2,
  // The predicate declined to decide; the inputs are malformed, not arbitraged.
  InvalidInput = 3,
};

enum class FeasibilityFailure : std::uint8_t {
  None = 0,
  // -- ModelIndependentArbitrage --
  NotStrictlyDecreasing = 1,       // condition 1
  SlopeAtZeroBelowMinusOne = 2,    // condition 2
  DuplicateStrikeDisagreement = 3, // two prices for one claim
  NegativePrice = 4,               // a call quoted below zero
  // -- WeakArbitrage --
  AboveConvexMinorant = 5, // condition 3
  // -- InvalidInput --
  NonFiniteQuote = 6,
  NonPositiveStrike = 7,
  UnsortedStrikes = 8,
  BadForwardOrDiscount = 9,
  ScratchTooSmall = 10,
};

inline constexpr std::uint32_t kNoQuoteIndex = 0xFFFFFFFFu;

// One hull vertex. Exposed only so the caller can size the scratch span; the
// contents are an implementation detail and are not read after the call.
struct FeasibilityNode {
  double x{0.0};                      // normalised strike K/F -- NOT log-moneyness
  double y{0.0};                      // minorant value at x
  std::uint32_t index{kNoQuoteIndex}; // source quote index, or kNoQuoteIndex for (0,1)
};

// Minimum `scratch` length for `n_quotes` inputs: every quote can be a hull
// vertex, plus the adjoined origin.
[[nodiscard]] constexpr std::size_t feasibility_scratch_size(std::size_t n_quotes) noexcept {
  return n_quotes + 1u;
}

struct FeasibilityReport {
  FeasibilityVerdict verdict{FeasibilityVerdict::Feasible};
  FeasibilityFailure failure{FeasibilityFailure::None};

  // Quotes that survived screening (two-sided-or-one-sided, finite, positive
  // strike, deduplicated). Zero quotes is vacuously Feasible.
  std::uint32_t n_points{0};

  // Condition-3 detail. `n_weak_points` is the TRUE count even when
  // `weak_strikes_out` was too short to receive them all; `n_weak_reported` is
  // how many indices were actually written. Both are populated whatever the
  // verdict, so a ModelIndependentArbitrage slice still reports its butterflies.
  std::uint32_t n_weak_points{0};
  std::uint32_t n_weak_reported{0};

  // First input index implicated by `failure`, or kNoQuoteIndex.
  std::uint32_t first_failure_index{kNoQuoteIndex};

  // ---- Numeric margins. All three are populated whenever the predicate got
  // far enough to compute them, so a caller can rank near-misses. ----

  // Condition 1: the smallest normalised price DROP across a minorant segment
  // inside [0, x_{n0 ^ n}]. Feasible needs > price_tol. Zero when there is no
  // such segment (no quotes).
  double min_drop{0.0};

  // Condition 2: R'(0+). Feasible needs >= -1, and the verdict applies
  // `price_tol` to the equivalent price form r_1 >= 1 - x_1, so a slope
  // marginally under -1 at a deep-in-the-money first strike is NOT a failure.
  // Zero when there are no quotes.
  double slope_at_zero{0.0};

  // Condition 3: max_i (r_i - R(x_i)) >= 0. Feasible needs <= price_tol.
  double max_excess{0.0};
};

// Decide whether any arbitrage-free model reproduces `quotes`.
//
// PRECONDITIONS (checked, never UB):
//   - `quotes` is sorted by ascending strike. Unsorted input returns
//     InvalidInput/UnsortedStrikes rather than being sorted internally: the
//     caller's slice is already strike-ordered, and sorting here would need
//     either an allocation or write access to the caller's data.
//   - `scratch.size() >= feasibility_scratch_size(quotes.size())`, else
//     InvalidInput/ScratchTooSmall. The span is written and read; its contents
//     on return are unspecified.
//   - `inputs.forward > 0` and `inputs.discount > 0`, both finite, else
//     InvalidInput/BadForwardOrDiscount.
//
// DEGENERATE INPUT POLICY (each case is tested):
//   - 0 quotes           -> Feasible, n_points 0. Nothing to contradict.
//   - 1 quote            -> conditions 1 and 2 still bind (condition 3 is
//                           vacuous), so a single quote below its intrinsic
//                           bound is still ModelIndependentArbitrage.
//   - duplicate strikes  -> equal prices collapse to one point; prices
//                           differing by more than price_tol are two prices for
//                           one claim, i.e.
//                           ModelIndependentArbitrage/DuplicateStrikeDisagreement.
//   - non-finite strike or price -> InvalidInput/NonFiniteQuote. Screened, not
//                           skipped: a NaN in the fit path is a defect upstream
//                           and must not be laundered into a verdict.
//   - strike <= 0        -> InvalidInput/NonPositiveStrike. x = 0 is the
//                           adjoined point; no listed contract lives there.
//   - price < 0          -> ModelIndependentArbitrage/NegativePrice. Being paid
//                           to hold a non-negative payoff is the definition.
//   - neither side flag  -> the row is not a quote; skipped, not an error.
//
// Screening and verdicts are decided in ascending input order, so the reported
// `failure` and `first_failure_index` are deterministic.
//
// @param weak_strikes_out optional sink for the input indices failing condition
//        3, written in ascending order and truncated to fit.
[[nodiscard]] FeasibilityReport
check_quote_feasibility(std::span<const CallQuote> quotes, const FeasibilityInputs &inputs,
                        std::span<FeasibilityNode> scratch,
                        std::span<std::uint32_t> weak_strikes_out = {}) noexcept;

[[nodiscard]] const char *to_string(FeasibilityVerdict verdict) noexcept;
[[nodiscard]] const char *to_string(FeasibilityFailure failure) noexcept;

} // namespace atx::vol::detail
