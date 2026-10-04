#pragma once

// atx::engine::alpha — platform-v8 lane YOPS kernels: the as-of rank family.
//
// Kakushadze's "101 Formulaic Alphas" (arXiv:1601.00991, appendix A.1) adjust
// every price of a formula's history as of the evaluation day: on day t the
// close of an earlier session s is close(s) adjusted for the splits and
// dividends up to t. A cross-sectional rank of such a price at session s is
// therefore a function of (s, t), not of s alone, and an operator that applies
// a time-series op to it (Ts_Rank(rank(low), 9), correlation(rank(open),
// rank(volume), 10), ...) cannot be written from the house point-in-time ops.
// The as-of family computes exactly that 2-D re-evaluation.
//
// ===========================================================================
//  PINNED SEMANTICS
// ===========================================================================
//  asof_rank_<outer>(x, w, [y,] d, j), d >= 1 the window, j >= 0 the lag, at
//  date t (NaN while t + 1 < d + j):
//    * the window is the d sessions s0 .. s0 + d - 1, s0 = t + 1 - d - j (it
//      ends j sessions before t);
//    * w* = per instrument the latest non-NaN w in rows s0 .. t (w is the
//      rebase factor, e.g. raw_close / close: constant between corporate
//      actions; a halted session at t borrows the last available factor);
//    * R[s] = the house cross-sectional rank (cs_rank_row: average ties, a
//      singleton 0.5) of x[s] * w* over its valid set — non-NaN products, and
//      under the VM's Cs eligibility mask the names eligible at s — NaN
//      outside it;
//    * out = the house <outer> (ts_value_at / ts_pair_at) over the d values
//      R[s0 .. s0 + d - 1] of each instrument: ts_rank / ts_min / decay_linear,
//      or correlation / covariance with y over the same d sessions. A NaN in
//      either window -> NaN; correlation's flat guard and cov's ddof 1 follow
//      the kernels (and the Engine's FlatGuard / RankTies policy).
//  With w == 1 the op is <outer>(rank(x), d) delayed by j, bit for bit (the
//  product x * 1 is exact); with d == 1 it is the as-of rank at lag j itself.
//  group_delay(g, d) is delay's shift kernel on a classifier (vm.hpp
//  eval_ts_lookback / ts_value_at TsDelay), so it needs no kernel here.
//
// The VM (vm.hpp eval_asof, contiguous date rows) and the StreamingEngine
// (streaming_engine.hpp exec_asof, a ring gathered chronologically) both call
// asof_rank_row below, so streaming == batch holds by construction; the oracle
// restates the rule independently (src/alpha/oracle_formulaic.cpp) and the
// differential suite (tests/alpha/alpha_formulaic_ops_test.cpp) proves
// VM == oracle bit for bit. The outer kernels are the batch per-cell kernels in
// every EvalMode (no online slide), so ResearchFast == AuditExact for this family.
//
// Names are distinct from oracle.hpp's detail::cs_* (a differential TU includes
// both headers in the shared detail namespace).

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/cs_ops.hpp"   // cs_rank_row, CsScratch, RankTies
#include "atx/engine/alpha/registry.hpp" // OpCode, asof_inner_op, asof_is_pair
#include "atx/engine/alpha/ts_ops.hpp"   // ts_value_at, ts_pair_at, FlatGuard

namespace atx::engine::alpha::detail {

// Reusable per-executor scratch (grown on demand, never shrunk).
struct AsofScratch {
  std::vector<atx::f64> factor;  // w* per instrument
  std::vector<atx::f64> prod;    // one window row of x * w*
  std::vector<atx::f64> rank;    // the d re-ranked window rows (row-major, n per row)
  std::vector<atx::usize> valid; // one row's valid set (ascending instrument index)
  CsScratch cs;                  // cs_rank_row's sort scratch
  std::vector<atx::f64> wa;      // ts kernel window scratch (>= d)
  std::vector<atx::f64> wb;
};

// Window d and lag j of an as-of instruction (Instr::imm[0], imm[1]). The
// typechecker proved both integers (d in [1, 65535], j in [0, 65535]); a value
// outside that range (a hand-built program) maps d to 0, which every executor
// treats as "all NaN", and j to 0, so the casts below are always defined.
struct AsofGeom {
  atx::usize d{0};
  atx::usize j{0};
};

[[nodiscard]] inline AsofGeom asof_geom(const std::array<atx::f64, 2> &imm) noexcept {
  constexpr atx::f64 kMax = 65535.0;
  AsofGeom g;
  if (imm[0] >= 1.0 && imm[0] <= kMax) {
    g.d = static_cast<atx::usize>(imm[0]);
  }
  if (imm[1] >= 0.0 && imm[1] <= kMax) {
    g.j = static_cast<atx::usize>(imm[1]);
  }
  return g;
}

// One output row (n instruments) of the as-of op `op` at a date whose window fits:
//   xw = rows s0 .. s0 + d - 1 of x (d * n cells, chronological),
//   ww = rows s0 .. t of w (rows_w * n cells, rows_w = d + j, the last row is t),
//   yw = rows s0 .. s0 + d - 1 of y (pair ops; empty otherwise),
//   mw = rows s0 .. s0 + d - 1 of the Cs eligibility mask (empty = all eligible).
// Writes every cell of `out`.
inline void asof_rank_row(OpCode op, std::span<const atx::f64> xw, std::span<const atx::f64> ww,
                          std::span<const atx::f64> yw, std::span<const atx::u8> mw,
                          atx::usize n, atx::usize d, std::span<atx::f64> out, AsofScratch &s,
                          RankTies ties, FlatGuard flat) {
  if (n == 0 || d == 0) {
    std::fill(out.begin(), out.end(), kCsNaN);
    return;
  }
  const atx::usize rows_w = ww.size() / n;
  // w*: newest -> oldest, the first non-NaN factor of each instrument.
  s.factor.resize(n);
  for (atx::usize i = 0; i < n; ++i) {
    atx::f64 f = kCsNaN;
    for (atx::usize r = rows_w; r > 0; --r) {
      const atx::f64 v = ww[(r - 1) * n + i];
      if (!cs_is_nan(v)) {
        f = v;
        break;
      }
    }
    s.factor[i] = f;
  }
  // The d re-ranked rows: the same valid scan as vm.hpp cs_one_date (ascending
  // index, non-NaN AND eligible), then the house rank kernel.
  s.prod.resize(n);
  s.rank.resize(d * n);
  for (atx::usize k = 0; k < d; ++k) {
    const std::span<const atx::f64> xr = xw.subspan(k * n, n);
    const std::span<const atx::u8> mr = mw.empty() ? mw : mw.subspan(k * n, n);
    const std::span<atx::f64> rr{s.rank.data() + k * n, n};
    s.valid.clear();
    for (atx::usize i = 0; i < n; ++i) {
      s.prod[i] = xr[i] * s.factor[i];
      rr[i] = kCsNaN;
      if (!cs_is_nan(s.prod[i]) && (mr.empty() || mr[i] != 0U)) {
        s.valid.push_back(i);
      }
    }
    cs_rank_row(std::span<const atx::f64>{s.prod.data(), n}, s.valid, rr, s.cs, ties);
  }
  // The outer house kernel over each instrument's d re-ranked values (stride n,
  // evaluated at the window's last row d - 1).
  if (s.wa.size() < d) {
    s.wa.resize(d);
    s.wb.resize(d);
  }
  const std::span<const atx::f64> rk{s.rank.data(), d * n};
  const OpCode inner = asof_inner_op(op);
  const bool pair = asof_is_pair(op);
  for (atx::usize i = 0; i < n; ++i) {
    out[i] = pair ? ts_pair_at(inner, rk, yw, d - 1, i, d, n, s.wa, s.wb, flat)
                  : ts_value_at(inner, rk, d - 1, i, d, n, s.wa, 0.0, flat);
  }
}

} // namespace atx::engine::alpha::detail
