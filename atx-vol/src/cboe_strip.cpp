// cboe_strip.hpp implementation -- the published discrete-strike variance sum.
//
// The header states the formula and the two readings this module takes where
// the published text is silent. This file is the mechanical transcription of
// it: validate the board once at the boundary, resolve K_0, walk each wing
// under the zero-bid rule, resolve dK over the SURVIVING strikes, sum, correct.
//
// Deliberately free of any dependency on the fitting stack: a basis measured
// between this and `var_swap_fair_strike` must not share code with it.

#include "atx/vol/cboe_strip.hpp"

#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Stop the wing walk after this many CONSECUTIVE zero-bid listed strikes. The
// published rule; named rather than spelled `2` at the comparison so the rule
// is greppable and so a reader does not have to decide whether the literal is
// the rule or an implementation detail.
constexpr std::size_t kZeroBidRunToTruncate = 2;

// A quote leg is admissible iff it has a live bid. Non-positive covers both the
// literal zero bid the methodology names and the negative bid a malformed feed
// can emit; the boundary validation below has already rejected non-finite.
[[nodiscard]] bool has_bid(double bid) noexcept { return bid > 0.0; }

[[nodiscard]] double mid_of(double bid, double ask) noexcept { return 0.5 * (bid + ask); }

// Every board field must be finite and non-negative, and an ask may not sit
// below its own bid. Validated ONCE here so the walk and the sum below can
// assume it -- an inverted or NaN quote that reached the sum would surface as a
// NaN strike, which is precisely the failure mode this module must not have.
[[nodiscard]] Status validate_leg(double bid, double ask, std::size_t index, const char *side) {
  if (!std::isfinite(bid) || !std::isfinite(ask) || bid < 0.0 || ask < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               std::string{"cboe_var_strike: non-finite or negative "} + side + " quote at board["
                   + std::to_string(index) + "]");
  }
  if (ask < bid) {
    return Err(ErrorCode::InvalidArgument,
               std::string{"cboe_var_strike: crossed "} + side + " quote at board["
                   + std::to_string(index) + "]");
  }
  return Ok();
}

[[nodiscard]] Status validate_board(std::span<const CboeStrikeQuote> board, double forward,
                                    double df, double maturity_t) {
  if (!std::isfinite(forward) || forward <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "cboe_var_strike: forward must be finite and positive");
  }
  if (!std::isfinite(df) || df <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "cboe_var_strike: discount factor must be finite and positive");
  }
  if (!std::isfinite(maturity_t) || maturity_t <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "cboe_var_strike: maturity_t must be finite and > 0");
  }
  // Two rows is the structural floor: dK is a difference, so one strike cannot
  // produce one.
  if (board.size() < 2u) {
    return Err(ErrorCode::InvalidArgument,
               "cboe_var_strike: board needs at least two listed strikes (dK is a difference)");
  }
  for (std::size_t i = 0; i < board.size(); ++i) {
    const CboeStrikeQuote &row = board[i];
    if (!std::isfinite(row.strike) || row.strike <= 0.0) {
      return Err(ErrorCode::InvalidArgument,
                 "cboe_var_strike: non-finite or non-positive strike at board["
                     + std::to_string(i) + "]");
    }
    // STRICTLY ascending: a duplicate strike would be counted twice by the sum
    // and would give its neighbours a zero-width dK, so it is a caller error
    // rather than something to silently coalesce.
    if (i > 0 && !(row.strike > board[i - 1u].strike)) {
      return Err(ErrorCode::InvalidArgument,
                 "cboe_var_strike: strikes must be strictly ascending; board["
                     + std::to_string(i) + "] is not");
    }
    if (const Status s = validate_leg(row.call_bid, row.call_ask, i, "call"); !s) {
      return s;
    }
    if (const Status s = validate_leg(row.put_bid, row.put_ask, i, "put"); !s) {
      return s;
    }
  }
  return Ok();
}

// K_0 = the largest listed strike <= F. See the header for why this module
// reads the published "first strike below F" inclusively.
//
// Linear rather than a binary search: this runs once per settlement, boards are
// hundreds of strikes, and a linear scan cannot get the tie at F == K wrong.
[[nodiscard]] Result<std::size_t> resolve_k0_index(std::span<const CboeStrikeQuote> board,
                                                   double forward) {
  bool found = false;
  std::size_t k0 = 0;
  for (std::size_t i = 0; i < board.size(); ++i) {
    if (board[i].strike <= forward) {
      k0 = i;
      found = true;
    } else {
      break; // ascending, so nothing beyond here can qualify
    }
  }
  if (!found) {
    return Err(ErrorCode::OutOfRange,
               "cboe_var_strike: forward is below the lowest listed strike, so no K0 exists");
  }
  return Ok(k0);
}

} // namespace

Result<CboeVarStrip> cboe_var_strike(std::span<const CboeStrikeQuote> board, double forward,
                                     double df, double maturity_t) {
  if (const Status s = validate_board(board, forward, df, maturity_t); !s) {
    return Err(s.error());
  }
  ATX_TRY(const std::size_t k0_index, resolve_k0_index(board, forward));

  CboeVarStrip out{};
  out.k0 = board[k0_index].strike;

  // ── Selection ────────────────────────────────────────────────────────────
  // Built ascending: puts are collected walking DOWN so they land descending,
  // and are appended in reverse below. `terms` carries only (strike, mid, leg)
  // at this stage; dK needs the whole surviving set and is filled afterwards.
  std::vector<CboeStripTerm> puts_descending;
  puts_descending.reserve(k0_index);
  {
    std::size_t zero_bid_run = 0;
    for (std::size_t step = 0; step < k0_index; ++step) {
      const std::size_t i = k0_index - 1u - step;
      const CboeStrikeQuote &row = board[i];
      if (!has_bid(row.put_bid)) {
        ++zero_bid_run;
        if (zero_bid_run >= kZeroBidRunToTruncate) {
          out.zero_bid_truncated_low = true;
          break;
        }
        continue;
      }
      zero_bid_run = 0;
      CboeStripTerm term{};
      term.strike = row.strike;
      term.mid = mid_of(row.put_bid, row.put_ask);
      term.leg = CboeStripLeg::Put;
      puts_descending.push_back(term);
    }
  }

  std::vector<CboeStripTerm> calls_ascending;
  calls_ascending.reserve(board.size() - k0_index);
  {
    std::size_t zero_bid_run = 0;
    for (std::size_t i = k0_index + 1u; i < board.size(); ++i) {
      const CboeStrikeQuote &row = board[i];
      if (!has_bid(row.call_bid)) {
        ++zero_bid_run;
        if (zero_bid_run >= kZeroBidRunToTruncate) {
          out.zero_bid_truncated_high = true;
          break;
        }
        continue;
      }
      zero_bid_run = 0;
      CboeStripTerm term{};
      term.strike = row.strike;
      term.mid = mid_of(row.call_bid, row.call_ask);
      term.leg = CboeStripLeg::Call;
      calls_ascending.push_back(term);
    }
  }

  out.n_puts = puts_descending.size();
  out.n_calls = calls_ascending.size();

  out.terms.reserve(out.n_puts + out.n_calls + 1u);
  for (std::size_t step = 0; step < puts_descending.size(); ++step) {
    out.terms.push_back(puts_descending[puts_descending.size() - 1u - step]);
  }
  {
    // K_0 is the one strike contributing BOTH legs, and it is exempt from the
    // zero-bid exclusion (header).
    const CboeStrikeQuote &row = board[k0_index];
    CboeStripTerm term{};
    term.strike = row.strike;
    term.mid = 0.5 * (mid_of(row.put_bid, row.put_ask) + mid_of(row.call_bid, row.call_ask));
    term.leg = CboeStripLeg::K0Average;
    out.terms.push_back(term);
  }
  for (const CboeStripTerm &term : calls_ascending) {
    out.terms.push_back(term);
  }

  // An all-bidless board truncates both wings immediately and leaves K_0 alone.
  // One strike has no dK under either the midpoint rule or the endpoint one, so
  // this is where that degenerate board becomes a Status instead of a NaN.
  const std::size_t n = out.terms.size();
  if (n < 2u) {
    return Err(ErrorCode::InvalidArgument,
               "cboe_var_strike: fewer than two strikes survived selection (every wing bidless); "
               "dK is undefined");
  }

  // ── dK, over the SURVIVING strikes ───────────────────────────────────────
  // Midpoint spacing in the interior; the published one-sided rule at each end
  // of the strip. See the header for why "each end of the STRIP" and not "each
  // end of the board".
  const double inv_df = 1.0 / df;
  for (std::size_t j = 0; j < n; ++j) {
    CboeStripTerm &term = out.terms[j];
    if (j == 0) {
      term.delta_k = out.terms[1].strike - out.terms[0].strike;
    } else if (j + 1u == n) {
      term.delta_k = out.terms[n - 1u].strike - out.terms[n - 2u].strike;
    } else {
      term.delta_k = 0.5 * (out.terms[j + 1u].strike - out.terms[j - 1u].strike);
    }
    term.contribution = (term.delta_k / (term.strike * term.strike)) * inv_df * term.mid;
  }

  double sum = 0.0;
  for (const CboeStripTerm &term : out.terms) {
    sum += term.contribution;
  }

  out.sum_term = (2.0 / maturity_t) * sum;
  const double f_over_k0_less_one = forward / out.k0 - 1.0;
  out.taylor_term = -(1.0 / maturity_t) * f_over_k0_less_one * f_over_k0_less_one;
  out.var_strike_dec = out.sum_term + out.taylor_term;

  out.k_lo = out.terms.front().strike;
  out.k_hi = out.terms.back().strike;

  // A board so thin that the Taylor correction outruns the whole sum. Real
  // boards do not do this; returning it would hand the caller a NaN
  // `vol_strike_dec` and let a negative variance propagate into a mark.
  if (out.var_strike_dec < 0.0) {
    return Err(ErrorCode::OutOfRange,
               "cboe_var_strike: resolved variance is negative ("
                   + std::to_string(out.var_strike_dec)
                   + "); the board is too sparse for its forward");
  }
  out.vol_strike_dec = std::sqrt(out.var_strike_dec);

  return Ok(std::move(out));
}

Result<CboeParametricBasis> cboe_parametric_basis(std::span<const CboeStrikeQuote> board,
                                                  double forward, double df, double maturity_t,
                                                  double parametric_var_dec) {
  if (!std::isfinite(parametric_var_dec) || parametric_var_dec < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "cboe_parametric_basis: parametric_var_dec must be finite and non-negative");
  }
  ATX_TRY(CboeVarStrip strip, cboe_var_strike(board, forward, df, maturity_t));

  CboeParametricBasis out{};
  out.cboe_var_dec = strip.var_strike_dec;
  out.parametric_var_dec = parametric_var_dec;
  out.basis_var_dec = strip.var_strike_dec - parametric_var_dec;
  out.basis_vol_dec = strip.vol_strike_dec - std::sqrt(parametric_var_dec);
  out.strip = std::move(strip);
  return Ok(std::move(out));
}

} // namespace atx::vol
