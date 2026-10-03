#pragma once

// atx::engine::combine -- the characteristic-covariance hedge of one rank-weighted characteristic
// portfolio, estimated causally from the panel's own returns.
//
// Daniel, Mota, Rottke and Santos (2020, "The Cross-Section of Risk and Returns", RFS 33(5)): a
// portfolio sorted on a characteristic carries the priced characteristic and the unpriced
// covariance of its names with the characteristic's own factor. A hedge portfolio that holds the
// characteristic fixed and is long the names whose loading on that factor the characteristic does
// not explain, scaled by the minimum-variance hedge ratio and subtracted, removes the unpriced
// part. Their weights come from past returns only, so the hedge is real time.
//
// Planes are date-major dates x names f64.
//   score    the characteristic; NaN = absent. R(d, i) is the centred tied rank of score over the
//            present names of date d (group_rerank.hpp's expression, in [-0.5, 0.5]); a date with
//            fewer than two present names has no rank.
//   returns  the session returns: returns(s, i) is name i's simple return over the sessions
//            (s - 1, s], NaN (or any non-finite value) when invalid. Row 0 is never read.
// Clock: decision d's book earns returns(d + lag, .). The pair of decision d' is known at decision
// d iff d' + lag <= d, so every estimate used at decision d reads only the pairs of the trailing
// `lookback` decisions ending d - lag: no session after d is read, and only session d among the
// sessions >= d - lag + 1 (char_cov_hedge's FutureReturnDoesNotChangePast pins this). Per decision
// d, every step on the pairs of that window:
//  1. sleeve s(d') = sum_i R(d', i) r_i(d'), r_i(d') = returns(d' + lag, i), over the ranked names
//     with a valid return (an invalid one adds 0); undefined at a decision without ranks.
//  2. beta_i(d) = the OLS slope (intercept included) of r_i on s over the window's pairs where both
//     are defined; NaN below min_pairs pairs or when s has no variance over them.
//  3. beta_perp(d): over the support (ranked names with a finite beta, at least two), the
//     least-squares residual of beta on [1, R(d, .)] (group_residualise.hpp): the part of the
//     loading the characteristic does not explain. H(d, i) is its centred tied rank over the
//     support, 0 for a ranked name outside it. H(d) is defined iff the support has two names and
//     the residual is not spanned (with two names it always is: [1, R] spans every two-name
//     cross-section).
//  4. hedge return g(d') = sum_i H(d', i) r_i(d') where H(d') is defined (as s).
//  5. h, the hedge ratio: at each block start j_b = lookback + lag - 1 + b * step (b = 0, 1, ...),
//     the OLS slope (intercept included) of s on g over the window's pairs where both are defined
//     (the minimum-variance hedge of the characteristic portfolio by the hedge portfolio); 0 below
//     min_pairs pairs or without variance of g. Held until the next block start; 0 before j_0.
//  6. out(d, i) = R(d, i) - h(d) H(d, i) for every ranked name: exactly R(d, i) while h is 0 or
//     H(d) is undefined. NaN for an unranked name.
// Per-name sums of the window are kept by adding the pair that enters and subtracting the pair that
// leaves (the same values, re-read from the planes), so a decision costs O(names log names).
// "No variance" is a centred sum of squares at most kHedgeVarianceFloor times the raw one (rounding
// of those running sums leaves a true zero a tiny residue).
//
// Header-only for the reason group_rerank.hpp gives: atx-impl instantiates these loops inside its
// optimised IC composition translation units, while the engine library builds unoptimised in Debug.

#include <cmath>            // std::isfinite, std::isnan
#include <initializer_list> // the trace shape check
#include <limits>           // std::numeric_limits
#include <new>              // std::bad_alloc
#include <span>             // std::span
#include <stdexcept>        // std::length_error
#include <vector>           // std::vector

#include "atx/core/error.hpp"                       // Result, Status
#include "atx/core/types.hpp"                       // f64, u8, u64, usize
#include "atx/engine/combine/group_rerank.hpp"      // for_each_centered_rank, RankedName
#include "atx/engine/combine/group_residualise.hpp" // residualise_in_place

namespace atx::engine::combine {

// The relative floor of "no variance" (see the header comment).
inline constexpr atx::f64 kHedgeVarianceFloor = 1e-12;
// Upper bound of lookback and step (a configuration beyond it is refused).
inline constexpr atx::usize kHedgeMaxWindow = 1U << 20;

struct CharCovHedgeConfig {
  atx::usize lookback = 252;  // decisions in each trailing window
  atx::usize lag = 2;         // a decision's book earns the session return `lag` sessions later
  atx::usize step = 21;       // decisions between two estimates of the hedge ratio
  atx::usize min_pairs = 126; // fewest pairs a beta or the hedge ratio is estimated on
};

// Optional diagnostics; each span is empty (not written) or written in full. One value per date:
// sleeve[d'] = s(d') and hedge[d'] = g(d') (NaN when undefined or not yet known at the last date),
// ratio[d] = the hedge ratio in force at d. One value per cell (dates x names): beta(d, i) = the
// loading step 2 used at d (NaN for an unranked name or one without an estimate).
struct CharCovHedgeTrace {
  std::span<atx::f64> sleeve, hedge, ratio, beta;
};

// Working storage reused across calls (one theme after another); contents are private.
struct CharCovHedgeScratch {
  std::vector<atx::f64> rank_ring, hedge_ring;       // (lag + 1) x names: R and H in flight
  std::vector<atx::u8> rank_defined, hedge_defined;   // lag + 1
  std::vector<atx::f64> sleeve, hedge;                // dates: s(d'), g(d')
  std::vector<atx::usize> count;                      // names: pairs in the window
  std::vector<atx::f64> sum_s, sum_r, sum_ss, sum_sr; // names: the window's sums
  std::vector<atx::f64> beta, dependent, column;      // names
  std::vector<atx::usize> support;                    // names
  std::vector<RankedName> row;                        // names
};

namespace hedge_detail {

[[nodiscard]] inline bool add_u64(atx::u64& total, atx::u64 n, atx::u64 width) noexcept {
  if (width != 0U && n > (std::numeric_limits<atx::u64>::max() - total) / width) return false;
  total += n * width;
  return true;
}

[[nodiscard]] inline bool valid_config(const CharCovHedgeConfig& cfg) noexcept {
  return cfg.lookback >= 2U && cfg.lookback <= kHedgeMaxWindow && cfg.lag >= 1U &&
         cfg.lag <= kHedgeMaxWindow && cfg.step >= 1U && cfg.step <= kHedgeMaxWindow &&
         cfg.min_pairs >= 2U && cfg.min_pairs <= cfg.lookback;
}

// OLS slope of y on x (intercept included) from the sums of n pairs; NaN when x has no variance.
[[nodiscard]] inline atx::f64 slope(atx::usize n, atx::f64 sx, atx::f64 sy, atx::f64 sxx,
                                    atx::f64 sxy) noexcept {
  const atx::f64 count = static_cast<atx::f64>(n);
  const atx::f64 variance = sxx - sx * sx / count;
  if (!(sxx > 0.0) || !(variance > kHedgeVarianceFloor * sxx))
    return std::numeric_limits<atx::f64>::quiet_NaN();
  return (sxy - sx * sy / count) / variance;
}

// sum_i weight(i) r(i) over the names whose weight and return are both finite.
[[nodiscard]] inline atx::f64 book_return(std::span<const atx::f64> weight,
                                          std::span<const atx::f64> r) noexcept {
  atx::f64 sum = 0.0;
  for (atx::usize i = 0; i < weight.size(); ++i)
    if (std::isfinite(weight[i]) && std::isfinite(r[i])) sum += weight[i] * r[i];
  return sum;
}

} // namespace hedge_detail

// Bytes CharCovHedgeScratch holds for `dates` x `names` under `cfg` (InvalidArgument for a config
// char_cov_hedge refuses or empty dimensions; OutOfRange on overflow).
[[nodiscard]] inline atx::core::Result<atx::u64> char_cov_hedge_scratch_bytes(
    atx::usize dates, atx::usize names, const CharCovHedgeConfig& cfg) {
  if (dates == 0U || names == 0U || !hedge_detail::valid_config(cfg))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "char cov hedge: dimensions or config");
  const atx::u64 ring = static_cast<atx::u64>(cfg.lag) + 1U;
  atx::u64 total = 0;
  const bool fits =
      hedge_detail::add_u64(total, ring * 2U, static_cast<atx::u64>(names) * sizeof(atx::f64)) &&
      hedge_detail::add_u64(total, ring, 2U) &&
      hedge_detail::add_u64(total, dates, 2U * sizeof(atx::f64)) &&
      hedge_detail::add_u64(total, names, 7U * sizeof(atx::f64) + 2U * sizeof(atx::usize) +
                                               sizeof(RankedName));
  if (!fits)
    return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                          "char cov hedge: scratch bytes overflow");
  return atx::core::Ok(total);
}

// The hedged characteristic of the header comment into `out` (same shape as score and returns, not
// aliasing either). Refuses (InvalidArgument), writing nothing: names == 0, planes of different
// sizes or not whole rows, a config outside lookback, lag, step in [1, kHedgeMaxWindow] (lookback
// >= 2) and 2 <= min_pairs <= lookback, or a non-empty trace span of the wrong length. OutOfRange
// when the scratch cannot be allocated. Deterministic and single-threaded.
[[nodiscard]] inline atx::core::Status char_cov_hedge(std::span<const atx::f64> score,
                                                      std::span<const atx::f64> returns,
                                                      atx::usize names,
                                                      const CharCovHedgeConfig& cfg,
                                                      std::span<atx::f64> out,
                                                      CharCovHedgeScratch& scratch,
                                                      CharCovHedgeTrace trace = {}) {
  namespace hd = hedge_detail;
  constexpr atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  if (names == 0U || score.size() != returns.size() || out.size() != score.size() ||
      score.size() % names != 0U || !hd::valid_config(cfg))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "char cov hedge: shape or config");
  const atx::usize dates = score.size() / names;
  for (const auto part : {trace.sleeve, trace.hedge, trace.ratio})
    if (!part.empty() && part.size() != dates)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "char cov hedge: trace shape");
  if (!trace.beta.empty() && trace.beta.size() != score.size())
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "char cov hedge: trace shape");
  const atx::usize ring = cfg.lag + 1U;
  auto& s = scratch;
  try {
    s.rank_ring.assign(ring * names, nan);
    s.hedge_ring.assign(ring * names, nan);
    s.rank_defined.assign(ring, 0U);
    s.hedge_defined.assign(ring, 0U);
    s.sleeve.assign(dates, nan);
    s.hedge.assign(dates, nan);
    s.count.assign(names, 0U);
    s.sum_s.assign(names, 0.0);
    s.sum_r.assign(names, 0.0);
    s.sum_ss.assign(names, 0.0);
    s.sum_sr.assign(names, 0.0);
    s.beta.assign(names, nan);
    s.dependent.assign(names, 0.0);
    s.column.assign(names, 0.0);
    s.support.reserve(names);
    s.row.reserve(names);
  } catch (const std::bad_alloc&) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                          "char cov hedge: scratch allocation failed");
  } catch (const std::length_error&) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                          "char cov hedge: scratch extent exceeded");
  }
  // The hedge ratio's window sums (pairs where s and g are both defined).
  atx::usize h_count = 0;
  atx::f64 h_s = 0.0, h_g = 0.0, h_gg = 0.0, h_sg = 0.0, ratio = 0.0;
  const atx::usize first_block = cfg.lookback + cfg.lag - 1U;
  const auto rank_row = [&](atx::usize slot) {
    return std::span<atx::f64>(s.rank_ring).subspan(slot * names, names);
  };
  const auto hedge_row = [&](atx::usize slot) {
    return std::span<atx::f64>(s.hedge_ring).subspan(slot * names, names);
  };
  // Adds (sign +1) or removes (sign -1) the pairs of decision p, whose returns are session p + lag.
  const auto move_pair = [&](atx::usize p, atx::f64 sign) {
    const auto r = returns.subspan((p + cfg.lag) * names, names);
    const atx::f64 x = s.sleeve[p];
    if (std::isfinite(x))
      for (atx::usize i = 0; i < names; ++i) {
        if (!std::isfinite(r[i])) continue;
        if (sign > 0.0) ++s.count[i]; else --s.count[i];
        s.sum_s[i] += sign * x;
        s.sum_r[i] += sign * r[i];
        s.sum_ss[i] += sign * x * x;
        s.sum_sr[i] += sign * x * r[i];
      }
    const atx::f64 y = s.hedge[p];
    if (std::isfinite(x) && std::isfinite(y)) {
      if (sign > 0.0) ++h_count; else --h_count;
      h_s += sign * x;
      h_g += sign * y;
      h_gg += sign * y * y;
      h_sg += sign * x * y;
    }
  };
  for (atx::usize d = 0; d < dates; ++d) {
    // 1 and 4: the pair of decision d - lag becomes known (its returns are session d).
    if (d >= cfg.lag) {
      const atx::usize p = d - cfg.lag, slot = p % ring;
      const auto r = returns.subspan(d * names, names);
      if (s.rank_defined[slot] != 0U) s.sleeve[p] = hd::book_return(rank_row(slot), r);
      if (s.hedge_defined[slot] != 0U) s.hedge[p] = hd::book_return(hedge_row(slot), r);
      move_pair(p, 1.0);
    }
    // The pair of decision d - lag - lookback leaves the window.
    if (d >= cfg.lag + cfg.lookback) move_pair(d - cfg.lag - cfg.lookback, -1.0);
    // 5: a block start re-estimates the hedge ratio on the window just formed.
    if (d >= first_block && (d - first_block) % cfg.step == 0U) {
      const atx::f64 b = h_count >= cfg.min_pairs ? hd::slope(h_count, h_g, h_s, h_gg, h_sg) : nan;
      ratio = std::isfinite(b) ? b : 0.0;
    }
    // R(d): the centred tied rank of the present names (fewer than two: none).
    const atx::usize slot = d % ring;
    const auto ranks = rank_row(slot), hedges = hedge_row(slot);
    for (atx::usize i = 0; i < names; ++i) ranks[i] = hedges[i] = nan;
    s.row.clear();
    const auto day = score.subspan(d * names, names);
    for (atx::usize i = 0; i < names; ++i)
      if (!std::isnan(day[i])) s.row.emplace_back(day[i], i);
    s.rank_defined[slot] = static_cast<atx::u8>(s.row.size() >= 2U);
    for_each_centered_rank(s.row, [&](atx::usize i, atx::f64 rank) { ranks[i] = rank; });
    // 2 and 3: the loadings on the window, the part the characteristic does not explain, its rank.
    s.support.clear();
    for (atx::usize i = 0; i < names; ++i) {
      s.beta[i] = nan;
      if (std::isnan(ranks[i])) continue;
      hedges[i] = 0.0;
      if (s.count[i] < cfg.min_pairs) continue;
      s.beta[i] = hd::slope(s.count[i], s.sum_s[i], s.sum_r[i], s.sum_ss[i], s.sum_sr[i]);
      if (std::isfinite(s.beta[i])) s.support.push_back(i);
    }
    bool defined = false;
    const atx::usize n = s.support.size();
    if (n >= 2U) {
      for (atx::usize k = 0; k < n; ++k) {
        s.dependent[k] = s.beta[s.support[k]];
        s.column[k] = ranks[s.support[k]];
      }
      ATX_TRY(const auto fit, residualise_in_place(std::span<atx::f64>(s.dependent).first(n),
                                                   std::span<atx::f64>(s.column).first(n)));
      if (!fit.spanned) {
        defined = true;
        s.row.clear();
        for (atx::usize k = 0; k < n; ++k) s.row.emplace_back(s.dependent[k], s.support[k]);
        for_each_centered_rank(s.row, [&](atx::usize i, atx::f64 rank) { hedges[i] = rank; });
      }
    }
    s.hedge_defined[slot] = static_cast<atx::u8>(defined);
    // 6: the hedged characteristic.
    const auto row_out = out.subspan(d * names, names);
    const bool hedged = defined && ratio != 0.0;
    for (atx::usize i = 0; i < names; ++i)
      row_out[i] = std::isnan(ranks[i]) ? nan : hedged ? ranks[i] - ratio * hedges[i] : ranks[i];
    if (!trace.ratio.empty()) trace.ratio[d] = ratio;
    if (!trace.beta.empty())
      for (atx::usize i = 0; i < names; ++i) trace.beta[d * names + i] = s.beta[i];
  }
  for (atx::usize d = 0; d < dates; ++d) {
    if (!trace.sleeve.empty()) trace.sleeve[d] = s.sleeve[d];
    if (!trace.hedge.empty()) trace.hedge[d] = s.hedge[d];
  }
  return atx::core::Ok();
}

} // namespace atx::engine::combine
