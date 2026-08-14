#include "fitting/quote_feasibility.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// Davis-Hobson Thm 3.1 predicate. See the header for the coordinate warning:
// `x` is the NORMALISED STRIKE K/F, never log-moneyness.
//
// Construction of the support function R, and why it is two O(n) passes:
//
//   R is the largest DECREASING CONVEX minorant of {(0,1)} u {(x_i, r_i)}. A
//   decreasing minorant can be no larger than the running minimum from the
//   LEFT (R(x_i) <= R(x_j) <= r_j for every x_j <= x_i), so R is the lower
//   convex hull of the prefix-minimised points. That hull is automatically
//   non-increasing: its slopes only increase along x, and its final slope
//   lands on the global minimum, hence is <= 0.
//
//   Pass 1 screens the input, applies the prefix minimum, and runs one
//   monotone chain.
//
//   Pass 2 re-walks the quotes against the finished hull to evaluate R(x_i).
//   A second pass rather than bookkeeping inside the chain because points pop
//   out of order: only a left-to-right sweep can report the offending strikes
//   in ascending order with their exact excess. It also decouples the verdict
//   from the chain's tie-breaking -- condition 3 asks whether a quote lies ON
//   R, not whether it is a hull VERTEX, so keeping or discarding collinear
//   points cannot change the answer (verified by mutation: flipping the pop
//   test to `cross > 0` leaves the whole suite green). They are kept anyway,
//   because it makes R at a quoted strike an exact table lookup instead of an
//   interpolation, and because it makes condition 1 name the FIRST strike of a
//   flat run rather than the last.

namespace atx::vol::detail {
namespace {

[[nodiscard]] FeasibilityReport terminal(FeasibilityVerdict verdict, FeasibilityFailure failure,
                                         std::uint32_t index, std::uint32_t n_points) noexcept {
  FeasibilityReport rep;
  rep.verdict = verdict;
  rep.failure = failure;
  rep.first_failure_index = index;
  rep.n_points = n_points;
  return rep;
}

// A quote row with neither side live is not a quote at all.
[[nodiscard]] bool is_quoted(const CallQuote &q) noexcept { return q.has_bid || q.has_ask; }

// Negative and non-finite tolerances are treated as zero rather than rejected:
// they are a caller-configuration slip, and the strict (tol = 0) predicate is
// the conservative reading of the theorem.
[[nodiscard]] double sane_tol(double tol) noexcept {
  return (std::isfinite(tol) && tol > 0.0) ? tol : 0.0;
}

} // namespace

FeasibilityReport check_quote_feasibility(std::span<const CallQuote> quotes,
                                          const FeasibilityInputs &inputs,
                                          std::span<FeasibilityNode> scratch,
                                          std::span<std::uint32_t> weak_strikes_out) noexcept {
  FeasibilityReport rep;

  // Nothing to contradict. Checked before the input validation so an empty
  // slice needs no forward, no discount and no scratch.
  if (quotes.empty()) {
    return rep;
  }
  if (!std::isfinite(inputs.forward) || inputs.forward <= 0.0 || !std::isfinite(inputs.discount) ||
      inputs.discount <= 0.0) {
    return terminal(FeasibilityVerdict::InvalidInput, FeasibilityFailure::BadForwardOrDiscount,
                    kNoQuoteIndex, 0u);
  }
  if (scratch.size() < feasibility_scratch_size(quotes.size())) {
    return terminal(FeasibilityVerdict::InvalidInput, FeasibilityFailure::ScratchTooSmall,
                    kNoQuoteIndex, 0u);
  }

  const double inv_forward = 1.0 / inputs.forward;
  const double inv_price = 1.0 / (inputs.discount * inputs.forward);
  const double tol = sane_tol(inputs.price_tol);
  const double zero_tol = sane_tol(inputs.zero_price_tol);

  // ── Pass 1: screen, prefix-minimise, build the hull ───────────────────────
  scratch[0] = FeasibilityNode{0.0, 1.0, kNoQuoteIndex}; // the adjoined point
  std::size_t top = 0;                                   // last occupied hull slot
  double prefix_min = 1.0;
  double prev_strike = 0.0;
  double prev_r = 0.0;
  bool have_prev = false;
  std::uint32_t n_points = 0;
  double x_last = 0.0;
  double x_stop = 0.0; // x_{n0 ^ n}: where condition 1 stops demanding strictness
  bool have_stop = false;

  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const CallQuote &q = quotes[i];
    const std::uint32_t idx = static_cast<std::uint32_t>(i);
    if (!is_quoted(q)) {
      continue;
    }
    if (!std::isfinite(q.strike) || !std::isfinite(q.price)) {
      return terminal(FeasibilityVerdict::InvalidInput, FeasibilityFailure::NonFiniteQuote, idx,
                      n_points);
    }
    if (q.strike <= 0.0) {
      return terminal(FeasibilityVerdict::InvalidInput, FeasibilityFailure::NonPositiveStrike, idx,
                      n_points);
    }
    if (have_prev && q.strike < prev_strike) {
      return terminal(FeasibilityVerdict::InvalidInput, FeasibilityFailure::UnsortedStrikes, idx,
                      n_points);
    }
    if (q.price < 0.0) {
      return terminal(FeasibilityVerdict::ModelIndependentArbitrage,
                      FeasibilityFailure::NegativePrice, idx, n_points);
    }

    const double r = q.price * inv_price;
    if (have_prev && q.strike == prev_strike) {
      if (std::fabs(r - prev_r) > tol) {
        return terminal(FeasibilityVerdict::ModelIndependentArbitrage,
                        FeasibilityFailure::DuplicateStrikeDisagreement, idx, n_points);
      }
      continue; // same claim, same price: one point, not two
    }

    const double x = q.strike * inv_forward;
    prefix_min = (r < prefix_min) ? r : prefix_min;
    const double y = prefix_min;

    // Monotone chain. Pop only on a STRICT right turn so collinear points stay.
    while (top >= 1u) {
      const FeasibilityNode &a = scratch[top - 1u];
      const FeasibilityNode &b = scratch[top];
      const double cross = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
      if (cross >= 0.0) {
        break;
      }
      --top;
    }
    ++top;
    scratch[top] = FeasibilityNode{x, y, idx};

    if (!have_stop && y <= zero_tol) {
      x_stop = x;
      have_stop = true;
    }
    prev_strike = q.strike;
    prev_r = r;
    have_prev = true;
    x_last = x;
    ++n_points;
  }

  rep.n_points = n_points;
  if (n_points == 0u) {
    return rep; // every row was unquoted: same vacuum as an empty span
  }
  if (!have_stop) {
    x_stop = x_last;
  }

  // ── Condition 2: R'(0+) ───────────────────────────────────────────────────
  rep.slope_at_zero = (scratch[1].y - 1.0) / scratch[1].x;

  // ── Condition 1: R strictly decreasing on [0, x_stop] ─────────────────────
  // Slopes increase along the hull, so it suffices to look at the drop of each
  // segment that starts inside the window; the smallest is the binding one.
  double min_drop = std::numeric_limits<double>::infinity();
  std::uint32_t drop_index = kNoQuoteIndex;
  for (std::size_t j = 1u; j <= top; ++j) {
    if (scratch[j - 1u].x >= x_stop) {
      break;
    }
    const double drop = scratch[j - 1u].y - scratch[j].y;
    if (drop < min_drop) {
      min_drop = drop;
      drop_index = scratch[j].index;
    }
  }
  rep.min_drop = std::isfinite(min_drop) ? min_drop : 0.0;

  // ── Pass 2 / condition 3: R(x_i) == r_i ───────────────────────────────────
  std::size_t cursor = 1u; // hull segment [cursor-1, cursor] currently spanning x
  double max_excess = 0.0;
  std::uint32_t first_weak = kNoQuoteIndex;
  std::uint32_t n_weak = 0u;
  std::uint32_t n_written = 0u;
  double seen_strike = 0.0;
  bool seen_any = false;

  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const CallQuote &q = quotes[i];
    if (!is_quoted(q)) {
      continue;
    }
    if (seen_any && q.strike == seen_strike) {
      continue; // collapsed in pass 1; scoring it twice would double-count
    }
    seen_strike = q.strike;
    seen_any = true;

    const double x = q.strike * inv_forward;
    const double r = q.price * inv_price;
    while (cursor < top && scratch[cursor].x < x) {
      ++cursor;
    }
    // Land exactly on a vertex rather than interpolating onto it: the division
    // below would otherwise leave a few ulps of excess on every hull point.
    double minorant = 0.0;
    if (x >= scratch[cursor].x) {
      minorant = scratch[cursor].y;
    } else {
      const FeasibilityNode &a = scratch[cursor - 1u];
      const FeasibilityNode &b = scratch[cursor];
      minorant = a.y + (b.y - a.y) * ((x - a.x) / (b.x - a.x));
    }

    const double excess = r - minorant;
    if (excess > max_excess) {
      max_excess = excess;
    }
    if (excess > tol) {
      ++n_weak;
      if (first_weak == kNoQuoteIndex) {
        first_weak = static_cast<std::uint32_t>(i);
      }
      if (n_written < weak_strikes_out.size()) {
        weak_strikes_out[n_written] = static_cast<std::uint32_t>(i);
        ++n_written;
      }
    }
  }
  rep.max_excess = max_excess;
  rep.n_weak_points = n_weak;
  rep.n_weak_reported = n_written;

  // Verdict precedence: a model-independent arbitrage is the stronger claim, so
  // conditions 1 and 2 outrank condition 3 when a slice fails both.
  if (rep.min_drop <= tol) {
    rep.verdict = FeasibilityVerdict::ModelIndependentArbitrage;
    rep.failure = FeasibilityFailure::NotStrictlyDecreasing;
    rep.first_failure_index = drop_index;
    // R'(0+) >= -1 is equivalent to r_1 >= 1 - x_1, and the slack is applied on
    // THAT form -- in price units -- deliberately. Tolerancing the slope
    // instead divides the slack by x_1, which tightens it without bound exactly
    // where it must not be: a deep-in-the-money call has x_1 << 1 and a mid
    // pinned to intrinsic to within a tick, so a slope tolerance turns every
    // penny of quote noise on the lowest listed strike into a verdict.
  } else if (scratch[1].y < 1.0 - scratch[1].x - tol) {
    rep.verdict = FeasibilityVerdict::ModelIndependentArbitrage;
    rep.failure = FeasibilityFailure::SlopeAtZeroBelowMinusOne;
    rep.first_failure_index = scratch[1].index;
  } else if (n_weak > 0u) {
    rep.verdict = FeasibilityVerdict::WeakArbitrage;
    rep.failure = FeasibilityFailure::AboveConvexMinorant;
    rep.first_failure_index = first_weak;
  }
  return rep;
}

const char *to_string(FeasibilityVerdict verdict) noexcept {
  switch (verdict) {
  case FeasibilityVerdict::Feasible:
    return "Feasible";
  case FeasibilityVerdict::WeakArbitrage:
    return "WeakArbitrage";
  case FeasibilityVerdict::ModelIndependentArbitrage:
    return "ModelIndependentArbitrage";
  case FeasibilityVerdict::InvalidInput:
    return "InvalidInput";
  }
  return "?";
}

const char *to_string(FeasibilityFailure failure) noexcept {
  switch (failure) {
  case FeasibilityFailure::None:
    return "None";
  case FeasibilityFailure::NotStrictlyDecreasing:
    return "NotStrictlyDecreasing";
  case FeasibilityFailure::SlopeAtZeroBelowMinusOne:
    return "SlopeAtZeroBelowMinusOne";
  case FeasibilityFailure::DuplicateStrikeDisagreement:
    return "DuplicateStrikeDisagreement";
  case FeasibilityFailure::NegativePrice:
    return "NegativePrice";
  case FeasibilityFailure::AboveConvexMinorant:
    return "AboveConvexMinorant";
  case FeasibilityFailure::NonFiniteQuote:
    return "NonFiniteQuote";
  case FeasibilityFailure::NonPositiveStrike:
    return "NonPositiveStrike";
  case FeasibilityFailure::UnsortedStrikes:
    return "UnsortedStrikes";
  case FeasibilityFailure::BadForwardOrDiscount:
    return "BadForwardOrDiscount";
  case FeasibilityFailure::ScratchTooSmall:
    return "ScratchTooSmall";
  }
  return "?";
}

} // namespace atx::vol::detail
