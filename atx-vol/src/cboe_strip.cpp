// cboe_strip.hpp implementation -- the published discrete-strike variance sum.
//
// The header states the formula, cites every rule to [CUR-M]/[CUR-V], and names
// the two conditions under which the index cannot be calculated. This file is
// the mechanical transcription: validate the board once at the boundary,
// resolve K_0, walk each wing under the zero-quote exclusion rule, refuse the
// board if either wing came back empty or K_0 is unquotable, resolve dK over the
// SURVIVING strikes, sum, correct.
//
// Deliberately free of any dependency on the fitting stack: a basis measured
// between this and `var_swap_fair_strike` must not share code with it.

#include "atx/vol/cboe_strip.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <utility>

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Stop the wing walk after this many CONSECUTIVE excluded listed strikes
// ([CUR-M] §3(a)(iii)). Named rather than spelled `2` at the comparison so the
// rule is greppable, and so a reader does not have to decide whether the literal
// is the rule or an implementation detail.
constexpr std::size_t kExcludedRunToTruncate = 2;

// A quote leg is admissible iff it is two-sided. [CUR-M] §3(a)(iii), as amended
// 2025-02-10: "Exclude any put option that has a bid price OR ASK PRICE equal to
// zero." Both halves matter and they exclude different things -- a zero bid is a
// series nobody will buy, a zero ask is a series nobody will sell, and a mid
// taken across either is not a price. `validate_leg` has already rejected
// negatives and non-finites, so this is a two-sided test and nothing more.
[[nodiscard]] bool is_admissible(double bid, double ask) noexcept {
  return bid > 0.0 && ask > 0.0;
}

// A NULL quote -- no market at all on this leg. Distinct from a real one-sided
// quote such as 0.00/0.30, whose midpoint of 0.15 is a genuine price. Only K_0
// has to tell the two apart ([CUR-M] §3(a)(ii)); everywhere else both are simply
// inadmissible.
[[nodiscard]] bool is_null_quote(double bid, double ask) noexcept {
  return bid <= 0.0 && ask <= 0.0;
}

[[nodiscard]] double mid_of(double bid, double ask) noexcept { return 0.5 * (bid + ask); }

// Every board field must be finite and non-negative, and a two-sided quote may
// not be crossed. Validated ONCE here so the walk and the sum below can assume
// it -- an inverted or NaN quote reaching the sum would surface as a NaN strike,
// which is precisely the failure mode this module must not have.
//
// The crossed test requires BOTH sides non-zero on purpose. `bid > 0, ask == 0`
// is a bid with no offer: a routine deep-wing state and a legitimate quote, not
// a malformed one. Rejecting the whole board for it would take the settlement
// diagnostic dark over a single illiquid series; the methodology instead
// excludes that one series and computes, which is what `is_admissible` does.
[[nodiscard]] Status validate_leg(double bid, double ask, std::size_t index, const char *side) {
  if (!std::isfinite(bid) || !std::isfinite(ask) || bid < 0.0 || ask < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               std::string{"cboe_var_strike: non-finite or negative "} + side + " quote at board["
                   + std::to_string(index) + "]");
  }
  if (bid > 0.0 && ask > 0.0 && ask < bid) {
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

// K_0 = the largest listed strike <= F ([CUR-M] §3(a): "equal to or otherwise
// immediately below F").
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

// [CUR-M] §3(a)(ii) / §5(b)(1): a NULL or crossed K_0 leg means there is no
// index. The crossed half is already rejected board-wide by `validate_leg` as a
// malformed input; what remains for K_0 is the null half.
//
// NULL only -- deliberately narrow. A one-sided K_0 quote (0.00/0.30, or
// 0.30/0.00) is not null: it is a real quote with a real midpoint, and Cboe's
// text does not say the walk's zero-bid-or-ask exclusion reaches the K_0 pair,
// which §3(a)(iii) selects OUTSIDE the walk. Rejecting it here would be this
// module inventing a rule. See the header's open-question paragraph.
[[nodiscard]] Status check_k0_quotable(const CboeStrikeQuote &row) {
  const char *bad = nullptr;
  if (is_null_quote(row.put_bid, row.put_ask)) {
    bad = "put";
  } else if (is_null_quote(row.call_bid, row.call_ask)) {
    bad = "call";
  }
  if (bad != nullptr) {
    return Err(ErrorCode::Unavailable,
               std::string{"cboe_var_strike: the K0 "} + bad
                   + " leg quote is null, so the index cannot be calculated (Cboe "
                     "Mathematics Methodology v5.0 2026-02-26 s3(a)(ii))");
  }
  return Ok();
}

} // namespace

Result<CboeVarStrip> cboe_var_strike(std::span<const CboeStrikeQuote> board, double forward,
                                     double df, double maturity_t, CboeVarStrip *diagnostic_out) {
  CboeVarStrip out{};
  // Every return below goes through one of these two, so the every-return-path
  // guarantee `diagnostic_out` advertises cannot be broken by adding a branch.
  const auto publish = [&]() {
    if (diagnostic_out != nullptr) {
      *diagnostic_out = out;
    }
  };
  const auto fail = [&](ErrorCode code, std::string message) {
    publish();
    return Err(code, std::move(message));
  };

  if (const Status s = validate_board(board, forward, df, maturity_t); !s) {
    publish();
    return Err(s.error());
  }
  const Result<std::size_t> k0_found = resolve_k0_index(board, forward);
  if (!k0_found) {
    publish();
    return Err(k0_found.error());
  }
  const std::size_t k0_index = *k0_found;
  out.k0 = board[k0_index].strike;

  if (const Status s = check_k0_quotable(board[k0_index]); !s) {
    publish();
    return Err(s.error());
  }

  // ── Selection ────────────────────────────────────────────────────────────
  // Built directly into `out.terms`: puts are collected walking DOWN, so they
  // land descending and the segment is reversed before K_0 and the calls are
  // appended. dK needs the whole surviving set and is filled afterwards.
  out.terms.reserve(board.size());
  {
    std::size_t excluded_run = 0;
    for (std::size_t step = 0; step < k0_index; ++step) {
      const std::size_t i = k0_index - 1u - step;
      const CboeStrikeQuote &row = board[i];
      if (!is_admissible(row.put_bid, row.put_ask)) {
        ++excluded_run;
        if (excluded_run >= kExcludedRunToTruncate) {
          // Only a stop with listed strikes still BELOW it refused anything;
          // see the flag's own contract in the header.
          out.zero_quote_truncated_low = i > 0;
          break;
        }
        continue;
      }
      excluded_run = 0;
      CboeStripTerm term{};
      term.strike = row.strike;
      term.mid = mid_of(row.put_bid, row.put_ask);
      term.leg = CboeStripLeg::Put;
      out.terms.push_back(term);
    }
  }
  out.n_puts = out.terms.size();
  std::reverse(out.terms.begin(), out.terms.end());

  {
    // K_0 is the one strike contributing BOTH legs, and it is exempt from the
    // walk ([CUR-M] §3(a)(iii)); `check_k0_quotable` above has already
    // established that both legs are quotable.
    const CboeStrikeQuote &row = board[k0_index];
    CboeStripTerm term{};
    term.strike = row.strike;
    term.mid = 0.5 * (mid_of(row.put_bid, row.put_ask) + mid_of(row.call_bid, row.call_ask));
    term.leg = CboeStripLeg::K0Average;
    out.terms.push_back(term);
  }

  {
    std::size_t excluded_run = 0;
    for (std::size_t i = k0_index + 1u; i < board.size(); ++i) {
      const CboeStrikeQuote &row = board[i];
      if (!is_admissible(row.call_bid, row.call_ask)) {
        ++excluded_run;
        if (excluded_run >= kExcludedRunToTruncate) {
          out.zero_quote_truncated_high = i + 1u < board.size();
          break;
        }
        continue;
      }
      excluded_run = 0;
      CboeStripTerm term{};
      term.strike = row.strike;
      term.mid = mid_of(row.call_bid, row.call_ask);
      term.leg = CboeStripLeg::Call;
      out.terms.push_back(term);
    }
  }
  out.n_calls = out.terms.size() - out.n_puts - 1u;

  // [CUR-M] §5(b)(2): an empty wing is not a thinner answer, it is no answer.
  // This also subsumes the structural "fewer than two strikes have no dK" case:
  // with both wings non-empty the strip is at least three strikes wide.
  if (out.n_puts == 0u || out.n_calls == 0u) {
    return fail(ErrorCode::Unavailable,
                std::string{"cboe_var_strike: every out-of-the-money "}
                    + (out.n_puts == 0u ? "put" : "call")
                    + " was excluded, so the index cannot be calculated (Cboe Mathematics "
                      "Methodology v5.0 2026-02-26 s5(b))");
  }

  // ── dK, over the SURVIVING strikes ───────────────────────────────────────
  // Midpoint spacing in the interior; the published one-sided rule at each end
  // of the strip. See the header for why "each end of the STRIP" and not "each
  // end of the board", and for the six published strikes that discriminate.
  const std::size_t n = out.terms.size();
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

  // A board so thin that the Taylor correction outruns the whole sum. Not a Cboe
  // rule -- a library-level guard: real boards do not do this, and returning it
  // would hand the caller a NaN `vol_strike_dec` and let a negative variance
  // propagate into a mark. The populated strip still travels out through
  // `diagnostic_out`, because the caller who hit this is exactly the one who
  // needs to see which strikes survived.
  if (out.var_strike_dec < 0.0) {
    return fail(ErrorCode::OutOfRange,
                "cboe_var_strike: resolved variance is negative ("
                    + std::to_string(out.var_strike_dec)
                    + "); the board is too sparse for its forward");
  }
  out.vol_strike_dec = std::sqrt(out.var_strike_dec);

  publish();
  return Ok(std::move(out));
}

Result<CboeParametricBasis> cboe_parametric_basis(std::span<const CboeStrikeQuote> board,
                                                  double forward, double df, double maturity_t,
                                                  double parametric_var_dec,
                                                  CboeVarStrip *diagnostic_out) {
  if (!std::isfinite(parametric_var_dec) || parametric_var_dec < 0.0) {
    if (diagnostic_out != nullptr) {
      *diagnostic_out = CboeVarStrip{};
    }
    return Err(ErrorCode::InvalidArgument,
               "cboe_parametric_basis: parametric_var_dec must be finite and non-negative");
  }
  ATX_TRY(CboeVarStrip strip, cboe_var_strike(board, forward, df, maturity_t, diagnostic_out));

  CboeParametricBasis out{};
  out.cboe_var_dec = strip.var_strike_dec;
  out.parametric_var_dec = parametric_var_dec;
  out.basis_var_dec = strip.var_strike_dec - parametric_var_dec;
  out.basis_vol_dec = strip.vol_strike_dec - std::sqrt(parametric_var_dec);
  out.strip = std::move(strip);
  return Ok(std::move(out));
}

} // namespace atx::vol
