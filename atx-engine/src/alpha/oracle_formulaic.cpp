// atx::engine::alpha — reference-oracle twins of the platform-v8 lane YOPS
// group_delay and as-of rank ops. The semantics are pinned in asof_ops.hpp; this
// file restates the rule INDEPENDENTLY (it does not include asof_ops.hpp) in the
// oracle's gather-then-compute style: per date the factor scan, every window row
// materialized and re-ranked with this oracle's cs_rank (O(n^2) average ties),
// then this oracle's own ts_unary_at / ts_binary_at over the ranked window — so
// the VM differential (alpha_formulaic_ops_test.cpp) is bit-exact and a real
// cross-check of the VM's row slicing, factor backfill, lag and warm-up handling.
// group_sum's twin (cs_group_sum) is a Cs kernel in oracle.cpp.
#include "atx/engine/alpha/oracle.hpp"

#include <span>   // std::span
#include <vector> // std::vector

namespace atx::engine::alpha {

namespace detail {

namespace {

// The house time-series op each as-of member applies to its ranked window.
[[nodiscard]] OpCode ref_asof_outer(OpCode op) noexcept {
  switch (op) {
  case OpCode::AsofRankTsRank:
    return OpCode::TsRank;
  case OpCode::AsofRankTsMin:
    return OpCode::TsMin;
  case OpCode::AsofRankDecayLinear:
    return OpCode::TsDecayLinear;
  case OpCode::AsofRankCorr:
    return OpCode::TsCorr;
  case OpCode::AsofRankCov:
    return OpCode::TsCov;
  default:
    ATX_UNREACHABLE();
  }
}

// An integer immediate in [lo, 65535], else `fallback` (a hand-built program).
[[nodiscard]] atx::usize ref_count(atx::f64 v, atx::f64 lo, atx::usize fallback) noexcept {
  return (v >= lo && v <= 65535.0) ? static_cast<atx::usize>(v) : fallback;
}

} // namespace

atx::core::Status Oracle::eval_formulaic(const Instr &in) {
  std::span<atx::f64> out = dst_col(in);
  const std::span<const atx::f64> x = src_col(in, 0);
  if (in.op == OpCode::GroupDelay) {
    // delay's shift on a classifier column; the window is operand 1 (a Const).
    const atx::usize d = window_of(src_col(in, 1));
    for (atx::usize j = 0; j < instruments_; ++j) {
      for (atx::usize t = 0; t < dates_; ++t) {
        out[t * instruments_ + j] = ts_unary_at(OpCode::TsDelay, x, t, j, d, 0.0);
      }
    }
    return atx::core::Ok();
  }
  const atx::usize d = ref_count(in.imm[0], 1.0, 0);
  const atx::usize lag = ref_count(in.imm[1], 0.0, 0);
  const std::span<const atx::f64> w = src_col(in, 1);
  const bool pair = in.op == OpCode::AsofRankCorr || in.op == OpCode::AsofRankCov;
  const std::span<const atx::f64> y = pair ? src_col(in, 2) : std::span<const atx::f64>{};
  const OpCode outer = ref_asof_outer(in.op);
  std::vector<atx::f64> factor(instruments_);
  std::vector<atx::f64> ranked(d * instruments_);
  for (atx::usize t = 0; t < dates_; ++t) {
    const std::span<atx::f64> orow = out.subspan(t * instruments_, instruments_);
    if (d == 0 || t + 1 < d + lag) {
      for (atx::f64 &c : orow) {
        c = kNaN;
      }
      continue;
    }
    const atx::usize s0 = t + 1 - d - lag;
    // The as-of factor: per instrument the newest non-NaN w in rows s0 .. t.
    for (atx::usize i = 0; i < instruments_; ++i) {
      factor[i] = kNaN;
      for (atx::usize s = t + 1; s > s0; --s) {
        const atx::f64 v = w[(s - 1) * instruments_ + i];
        if (!is_nan(v)) {
          factor[i] = v;
          break;
        }
      }
    }
    // Rows s0 .. s0 + d - 1 re-ranked on the factor of date t.
    for (atx::usize k = 0; k < d; ++k) {
      std::vector<atx::f64> rebased(instruments_);
      std::vector<atx::usize> valid;
      for (atx::usize i = 0; i < instruments_; ++i) {
        rebased[i] = x[(s0 + k) * instruments_ + i] * factor[i];
        if (!is_nan(rebased[i])) {
          valid.push_back(i);
        }
      }
      const std::span<atx::f64> rrow{ranked.data() + k * instruments_, instruments_};
      for (atx::f64 &c : rrow) {
        c = kNaN;
      }
      cs_rank(rebased, valid, rrow);
    }
    // The outer op at the window's last row, over the ranked window (and y's).
    const std::span<const atx::f64> rk{ranked};
    for (atx::usize i = 0; i < instruments_; ++i) {
      orow[i] = pair ? ts_binary_at(outer, rk, y.subspan(s0 * instruments_, d * instruments_),
                                    d - 1, i, d)
                     : ts_unary_at(outer, rk, d - 1, i, d, 0.0);
    }
  }
  return atx::core::Ok();
}

} // namespace detail

} // namespace atx::engine::alpha
