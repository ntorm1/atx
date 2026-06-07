#pragma once

// atx::engine::alpha — tree-walking reference oracle (P3-5).
//
// `evaluate_reference` executes a linearized `Program` over a `Panel` using the
// SIMPLEST obviously-correct model: walk `program.code` once, top-to-bottom,
// with every live SlotId holding a WHOLE `dates*instruments` f64 buffer. Each
// instruction reads its operand buffers and writes its `dst` buffer through a
// per-op kernel. The result `SignalSet` is the canonical answer the fast
// vectorized VM (P3-6) must reproduce BIT-FOR-BIT — so every numeric policy
// (NaN propagation, min_periods, cross-sectional tie-break, std ddof, the
// CsNeutG simplification) is PINNED here and shared.
//
// Public API:
//   Result<SignalSet> evaluate_reference(const Program&, const Panel&);
//
// SAFETY (topological execution): the Program's instruction stream is
// topologically ordered — every `src` slot is produced by an EARLIER instr (the
// linearizer emits in DAG NodeId order; see bytecode.hpp). The oracle therefore
// reads a slot only after the instruction that wrote it, so a single forward
// pass is correct with no scheduling.
//
// ===========================================================================
//  PINNED SEMANTIC CONTRACT (the differential reference — read before changing)
// ===========================================================================
//  * Representation: each slot buffer is date-major (`date*instruments + inst`).
//    A Scalar fills the whole buffer; a CrossSection (Cs*) result is written one
//    date-row at a time; a Panel (Ts*) fills naturally.
//  * NaN propagation: any element-wise op with a NaN operand yields NaN. min/max
//    yield NaN if EITHER operand is NaN (NOT std::min/max's pick-the-non-NaN).
//  * Masks are f64 1.0/0.0; NaN if any comparand is NaN. Logical &&/|| treat any
//    finite non-zero as true, 0 as false, NaN -> NaN. !x == 1-x for a 0/1 mask,
//    NaN -> NaN. Select(c,a,b): NaN c -> NaN, else c!=0 ? a : b per cell.
//  * Window operand convention: a Ts* op's window `d` is its LAST operand's
//    scalar value (read from that slot's [0] cell, since a scalar fills the
//    whole buffer), truncated toward zero; d<=0 -> all NaN. A Cs* op's scalar
//    parameter (CsScale `a`) is likewise read from its 2nd operand's [0] cell.
//  * Cross-sectional ops operate per date-row over the VALID SET (in-universe in
//    the Panel-derived mask AND non-NaN at that date); out-of-set cells -> NaN.
//  * Time-series ops are causal: trailing window [t-d+1, t] down each instrument
//    column. min_periods policy (min_periods()): FULL window by default — fewer
//    than `d` observations OR any NaN in the window -> NaN; delay/delta need only
//    the single shifted observation.
//  * Std/var ddof: SAMPLE (ddof=1) for Ts std/var/zscore/skew/kurt and Cs zscore
//    (documented; the VM must match). corr/cov use the population cross-moment
//    form normalized so corr in [-1,1].
//  * CsRank: ordinal percentile in [0,1] over the valid set, deterministic
//    tie-break by ascending instrument index (NaNs last). NOT average rank.
//  * CsNeutG/indneutralize/group_neutralize: implemented as per-group DEMEAN
//    (the regression residual on pure group-dummy design equals demeaning), so
//    CsDemeanG == CsNeutG here. RESIDUAL: a full WLS residualizer with extra
//    regressors is out of scope for the reference.
//
// Header-only; every free function is `inline`. The oracle is the COLD,
// obviously-correct path — clarity over speed.

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"

namespace atx::engine::alpha {

namespace detail {

inline constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] inline bool is_nan(atx::f64 x) noexcept { return std::isnan(x); }

// A mask cell is "true" iff finite and non-zero; NaN is neither true nor false
// (callers handle NaN before consulting truth).
[[nodiscard]] inline bool mask_true(atx::f64 x) noexcept { return x != 0.0 && !is_nan(x); }

// Read a scalar operand (window size / scale factor) from a slot whose whole
// buffer was filled with one broadcast value: cell [0] is representative.
[[nodiscard]] inline atx::f64 scalar_of(std::span<const atx::f64> col) noexcept {
  return col.empty() ? kNaN : col.front();
}

// Window size from a Ts op's last operand, truncated toward zero. <=0 or NaN
// yields 0 (the kernels then emit all-NaN, the documented degenerate case).
[[nodiscard]] inline atx::usize window_of(std::span<const atx::f64> col) noexcept {
  const atx::f64 v = scalar_of(col);
  if (is_nan(v) || v < 1.0) {
    return 0;
  }
  return static_cast<atx::usize>(v);
}

// ===========================================================================
//  min_periods policy — centralized here (the registry has no min_periods field
//  yet; RESIDUAL: promote into OpSig). Returns the number of valid trailing
//  observations a Ts op requires in its window to emit a non-NaN value.
//  Default == the full window `d`; delay/delta need only the single shifted obs.
// ===========================================================================
[[nodiscard]] inline atx::usize min_periods(OpCode op, atx::usize d) noexcept {
  switch (op) {
  case OpCode::TsDelay:
  case OpCode::TsDelta:
    return 1; // need only x[t-d]
  default:
    return d; // full-window policy for every other Ts op
  }
}

// ===========================================================================
//  Element-wise kernels (unary / binary). Each writes `out[i]` from operand
//  cells; NaN propagates per IEEE for + - * /, explicitly for min/max.
// ===========================================================================

template <class F>
inline void map_unary(std::span<const atx::f64> a, std::span<atx::f64> out, F f) {
  for (atx::usize i = 0; i < out.size(); ++i) {
    out[i] = f(a[i]);
  }
}

template <class F>
inline void map_binary(std::span<const atx::f64> a, std::span<const atx::f64> b,
                       std::span<atx::f64> out, F f) {
  for (atx::usize i = 0; i < out.size(); ++i) {
    out[i] = f(a[i], b[i]);
  }
}

[[nodiscard]] inline atx::f64 op_min(atx::f64 a, atx::f64 b) noexcept {
  if (is_nan(a) || is_nan(b)) {
    return kNaN;
  }
  return a < b ? a : b;
}

[[nodiscard]] inline atx::f64 op_max(atx::f64 a, atx::f64 b) noexcept {
  if (is_nan(a) || is_nan(b)) {
    return kNaN;
  }
  return a > b ? a : b;
}

[[nodiscard]] inline atx::f64 op_sign(atx::f64 a) noexcept {
  if (is_nan(a)) {
    return kNaN;
  }
  return (a > 0.0) - (a < 0.0);
}

// sigmoid(x) = 1/(1+exp(-x)) (P3b-2). NaN -> NaN naturally (exp/div propagate).
// Bit-identical to vm.hpp's Sigmoid lambda by construction.
[[nodiscard]] inline atx::f64 op_sigmoid(atx::f64 a) noexcept { return 1.0 / (1.0 + std::exp(-a)); }

// signedpower(x, e) = sign(x) * |x|^e (Alpha101 SignedPower).
[[nodiscard]] inline atx::f64 op_spow(atx::f64 a, atx::f64 e) noexcept {
  if (is_nan(a) || is_nan(e)) {
    return kNaN;
  }
  return op_sign(a) * std::pow(std::fabs(a), e);
}

// Comparison -> mask (NaN if either operand is NaN).
template <class Cmp>
inline void map_cmp(std::span<const atx::f64> a, std::span<const atx::f64> b,
                    std::span<atx::f64> out, Cmp cmp) {
  for (atx::usize i = 0; i < out.size(); ++i) {
    out[i] = (is_nan(a[i]) || is_nan(b[i])) ? kNaN : (cmp(a[i], b[i]) ? 1.0 : 0.0);
  }
}

[[nodiscard]] inline atx::f64 op_and(atx::f64 a, atx::f64 b) noexcept {
  if (is_nan(a) || is_nan(b)) {
    return kNaN;
  }
  return (mask_true(a) && mask_true(b)) ? 1.0 : 0.0;
}

[[nodiscard]] inline atx::f64 op_or(atx::f64 a, atx::f64 b) noexcept {
  if (is_nan(a) || is_nan(b)) {
    return kNaN;
  }
  return (mask_true(a) || mask_true(b)) ? 1.0 : 0.0;
}

[[nodiscard]] inline atx::f64 op_not(atx::f64 a) noexcept {
  return is_nan(a) ? kNaN : (mask_true(a) ? 0.0 : 1.0);
}

[[nodiscard]] inline atx::f64 op_select(atx::f64 c, atx::f64 a, atx::f64 b) noexcept {
  if (is_nan(c)) {
    return kNaN;
  }
  return mask_true(c) ? a : b;
}

// ===========================================================================
//  Cross-sectional helpers — operate on ONE date-row's valid set.
// ===========================================================================

// Per-date validity: in the source mask (mask col is 1.0 in-universe, NaN out)
// AND the value is non-NaN. The oracle pre-bakes the universe into a mask buffer
// so the kernels never touch the Panel directly.
[[nodiscard]] inline bool cell_valid(atx::f64 mask, atx::f64 value) noexcept {
  return mask_true(mask) && !is_nan(value);
}

[[nodiscard]] inline atx::f64 mean_of(std::span<const atx::f64> xs) noexcept {
  if (xs.empty()) {
    return kNaN;
  }
  atx::f64 sum = 0.0;
  for (const atx::f64 v : xs) {
    sum += v;
  }
  return sum / static_cast<atx::f64>(xs.size());
}

// Sample (ddof=1) standard deviation; NaN if fewer than 2 observations.
[[nodiscard]] inline atx::f64 sample_std(std::span<const atx::f64> xs, atx::f64 mean) noexcept {
  if (xs.size() < 2) {
    return kNaN;
  }
  atx::f64 ss = 0.0;
  for (const atx::f64 v : xs) {
    const atx::f64 d = v - mean;
    ss += d * d;
  }
  return std::sqrt(ss / static_cast<atx::f64>(xs.size() - 1));
}

} // namespace detail

// =========================================================================
//  Oracle — the executor. Holds the SlotPool, the per-instr dispatch, and the
//  pre-baked LoadField mask. Decomposed into per-family member helpers so each
//  stays well under the 60-line cap and the dispatch switch stays exhaustive.
// =========================================================================

namespace detail {

class Oracle {
public:
  Oracle(const Program &prog, const Panel &panel)
      : prog_{prog}, panel_{panel}, dates_{panel.dates()}, instruments_{panel.instruments()},
        cells_{panel.dates() * panel.instruments()},
        pool_{prog.num_slots == 0 ? atx::usize{1} : prog.num_slots,
              cells_ == 0 ? atx::usize{1} : cells_} {}

  [[nodiscard]] atx::core::Result<SignalSet> run() {
    SignalSet out;
    out.dates = dates_;
    out.instruments = instruments_;
    out.alphas.resize(prog_.roots.size());
    for (atx::usize r = 0; r < prog_.roots.size(); ++r) {
      out.alphas[r].name = prog_.roots[r].name;
      out.alphas[r].values.assign(cells_, detail::kNaN);
    }

    // The Program's SlotIds are pre-allocated by the linearizer (recycled via a
    // free-list; a slot is reused only AFTER its Free). They index directly into
    // the pool's pre-sized buffer — no remapping. acquire()/release() are called
    // purely to honor the pool's live-count precondition (the over-acquire
    // ATX_ASSERT): each value-producing instr acquires, each Free releases.
    // LIVENESS CONTRACT for multi-output nodes (mirrors vm.hpp's loop exactly):
    //   * Split2 goes through dispatch with ONE acquire; its block columns are
    //     accessed via pool_.column(in.dst+k) which is within the pre-sized pool.
    //   * Pin is handled here (before dispatch) with ONE acquire; it copies
    //     pool_.column(src[0]+param) -> pool_.column(dst).
    //   * Free always calls release once regardless of the block width (n_out).
    for (const Instr &in : prog_.code) {
      if (in.op == OpCode::Free) {
        pool_.release(in.dst);
        continue;
      }
      if (in.op == OpCode::StoreAlpha) {
        store_alpha(in, out);
        continue;
      }
      if (in.op == OpCode::Pin) {
        // Project one output of the parent block into this node's own slot.
        (void)pool_.acquire();
        const std::span<const atx::f64> src = pool_.column(in.src[0] + in.param);
        const std::span<atx::f64> dst_span = pool_.column(in.dst);
        for (atx::usize i = 0; i < cells_; ++i) {
          dst_span[i] = src[i];
        }
        continue;
      }
      (void)pool_.acquire();
      if (const atx::core::Status s = dispatch(in); !s) {
        return atx::core::Err(s.error());
      }
    }
    return atx::core::Ok(std::move(out));
  }

private:
  // The Program SlotId indexes the pool buffer directly (see run()).
  [[nodiscard]] std::span<atx::f64> dst_col(const Instr &in) { return pool_.column(in.dst); }
  [[nodiscard]] std::span<const atx::f64> src_col(const Instr &in, atx::usize k) const {
    return pool_.column(in.src.at(k));
  }

  // ---- StoreAlpha ---------------------------------------------------------
  void store_alpha(const Instr &in, SignalSet &out) {
    const std::span<const atx::f64> src = src_col(in, 0);
    std::vector<atx::f64> &dst = out.alphas.at(in.param).values;
    for (atx::usize i = 0; i < cells_; ++i) {
      dst[i] = src[i];
    }
  }

  // =======================================================================
  //  dispatch — the canonical EXHAUSTIVE switch over OpCode (NO default).
  //  StoreAlpha/Free are handled by the caller; they appear here only to keep
  //  the switch total and are unreachable (ATX_UNREACHABLE).
  // =======================================================================
  [[nodiscard]] atx::core::Status dispatch(const Instr &in) {
    switch (in.op) {
    case OpCode::LoadField:
      return eval_load_field(in);
    case OpCode::Const:
      return eval_const(in);
    case OpCode::Add:
    case OpCode::Sub:
    case OpCode::Mul:
    case OpCode::Div:
    case OpCode::Pow:
    case OpCode::Spow:
    case OpCode::MinP:
    case OpCode::MaxP:
      return eval_binary(in);
    case OpCode::Neg:
    case OpCode::Abs:
    case OpCode::Sign:
    case OpCode::Log:
    case OpCode::Sigmoid:
    case OpCode::Tanh:
      return eval_unary(in);
    case OpCode::CmpLt:
    case OpCode::CmpGt:
    case OpCode::CmpLe:
    case OpCode::CmpGe:
    case OpCode::CmpEq:
    case OpCode::CmpNe:
      return eval_cmp(in);
    case OpCode::And:
    case OpCode::Or:
      return eval_logical(in);
    case OpCode::Not:
      return eval_not(in);
    case OpCode::Select:
      return eval_select(in);
    case OpCode::CsRank:
    case OpCode::CsZscore:
    case OpCode::CsScale:
    case OpCode::CsNormalize:
    case OpCode::CsWinsorize:
    case OpCode::CsDemeanG:
    case OpCode::CsNeutG:
    case OpCode::CsRankG:
    case OpCode::CsZscoreG:
    case OpCode::CsCountG:
    case OpCode::CsMeanG:
    case OpCode::CsScaleG:
      return eval_cross_section(in);
    case OpCode::TsDelay:
    case OpCode::TsDelta:
    case OpCode::TsSum:
    case OpCode::TsMean:
    case OpCode::TsStd:
    case OpCode::TsVar:
    case OpCode::TsMin:
    case OpCode::TsMax:
    case OpCode::TsArgMin:
    case OpCode::TsArgMax:
    case OpCode::TsRank:
    case OpCode::TsCorr:
    case OpCode::TsCov:
    case OpCode::TsProduct:
    case OpCode::TsDecayLinear:
    case OpCode::TsEma:
    case OpCode::TsWma:
    case OpCode::TsSkew:
    case OpCode::TsKurt:
    case OpCode::TsMed:
    case OpCode::TsMad:
    case OpCode::TsSlope:
    case OpCode::TsRsquare:
    case OpCode::TsResid:
    case OpCode::TsZscore:
    case OpCode::TsBackfill:
    case OpCode::TsAvDiff:
    case OpCode::TsQuantile:
    case OpCode::TsScale:
    case OpCode::TsCountNans:
      return eval_time_series(in);
    case OpCode::TradeWhen:
    case OpCode::Hump:
    case OpCode::KalmanLevel:
    case OpCode::OuFilter:
      return eval_recurrence(in);
    case OpCode::Split2:
      return eval_split2(in);
    case OpCode::KalmanReg:
      return eval_kalman_reg(in);
    case OpCode::Pin:
    case OpCode::StoreAlpha:
    case OpCode::Free:
      ATX_UNREACHABLE(); // Pin/StoreAlpha/Free handled by run(); never dispatched
    }
    ATX_UNREACHABLE(); // exhaustive switch — no valid fallthrough
  }

  // ---- leaves -------------------------------------------------------------
  [[nodiscard]] atx::core::Status eval_const(const Instr &in) {
    std::span<atx::f64> out = dst_col(in);
    for (atx::f64 &c : out) {
      c = in.imm[0];
    }
    return atx::core::Ok();
  }

  // LoadField copies the field, NaN-ing any out-of-universe cell (PIT).
  [[nodiscard]] atx::core::Status eval_load_field(const Instr &in) {
    std::span<atx::f64> out = dst_col(in);
    const std::span<const atx::f64> field = panel_.field_all(static_cast<FieldId>(in.param));
    for (atx::usize d = 0; d < dates_; ++d) {
      for (atx::usize j = 0; j < instruments_; ++j) {
        const atx::usize idx = d * instruments_ + j;
        out[idx] = panel_.in_universe(d, j) ? field[idx] : detail::kNaN;
      }
    }
    return atx::core::Ok();
  }

  // ---- element-wise -------------------------------------------------------
  [[nodiscard]] atx::core::Status eval_unary(const Instr &in) {
    const std::span<const atx::f64> a = src_col(in, 0);
    std::span<atx::f64> out = dst_col(in);
    switch (in.op) {
    case OpCode::Neg:
      detail::map_unary(a, out, [](atx::f64 x) { return -x; });
      break;
    case OpCode::Abs:
      detail::map_unary(a, out, [](atx::f64 x) { return std::fabs(x); });
      break;
    case OpCode::Sign:
      detail::map_unary(a, out, detail::op_sign);
      break;
    case OpCode::Log:
      detail::map_unary(a, out, [](atx::f64 x) { return std::log(x); });
      break;
    case OpCode::Sigmoid:
      detail::map_unary(a, out, detail::op_sigmoid);
      break;
    case OpCode::Tanh:
      detail::map_unary(a, out, [](atx::f64 x) { return std::tanh(x); });
      break;
    default:
      ATX_UNREACHABLE();
    }
    return atx::core::Ok();
  }

  [[nodiscard]] atx::core::Status eval_binary(const Instr &in) {
    const std::span<const atx::f64> a = src_col(in, 0);
    const std::span<const atx::f64> b = src_col(in, 1);
    std::span<atx::f64> out = dst_col(in);
    switch (in.op) {
    case OpCode::Add:
      detail::map_binary(a, b, out, [](atx::f64 x, atx::f64 y) { return x + y; });
      break;
    case OpCode::Sub:
      detail::map_binary(a, b, out, [](atx::f64 x, atx::f64 y) { return x - y; });
      break;
    case OpCode::Mul:
      detail::map_binary(a, b, out, [](atx::f64 x, atx::f64 y) { return x * y; });
      break;
    case OpCode::Div:
      detail::map_binary(a, b, out, [](atx::f64 x, atx::f64 y) { return x / y; });
      break;
    case OpCode::Pow:
      detail::map_binary(a, b, out, [](atx::f64 x, atx::f64 y) { return std::pow(x, y); });
      break;
    case OpCode::Spow:
      detail::map_binary(a, b, out, detail::op_spow);
      break;
    case OpCode::MinP:
      detail::map_binary(a, b, out, detail::op_min);
      break;
    case OpCode::MaxP:
      detail::map_binary(a, b, out, detail::op_max);
      break;
    default:
      ATX_UNREACHABLE();
    }
    return atx::core::Ok();
  }

  [[nodiscard]] atx::core::Status eval_cmp(const Instr &in) {
    const std::span<const atx::f64> a = src_col(in, 0);
    const std::span<const atx::f64> b = src_col(in, 1);
    std::span<atx::f64> out = dst_col(in);
    switch (in.op) {
    case OpCode::CmpLt:
      detail::map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) { return x < y; });
      break;
    case OpCode::CmpGt:
      detail::map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) { return x > y; });
      break;
    case OpCode::CmpLe:
      detail::map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) { return x <= y; });
      break;
    case OpCode::CmpGe:
      detail::map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) { return x >= y; });
      break;
    case OpCode::CmpEq:
      detail::map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) { return x == y; });
      break;
    case OpCode::CmpNe:
      detail::map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) { return x != y; });
      break;
    default:
      ATX_UNREACHABLE();
    }
    return atx::core::Ok();
  }

  [[nodiscard]] atx::core::Status eval_logical(const Instr &in) {
    const std::span<const atx::f64> a = src_col(in, 0);
    const std::span<const atx::f64> b = src_col(in, 1);
    std::span<atx::f64> out = dst_col(in);
    detail::map_binary(a, b, out, in.op == OpCode::And ? detail::op_and : detail::op_or);
    return atx::core::Ok();
  }

  [[nodiscard]] atx::core::Status eval_not(const Instr &in) {
    detail::map_unary(src_col(in, 0), dst_col(in), detail::op_not);
    return atx::core::Ok();
  }

  [[nodiscard]] atx::core::Status eval_select(const Instr &in) {
    const std::span<const atx::f64> c = src_col(in, 0);
    const std::span<const atx::f64> a = src_col(in, 1);
    const std::span<const atx::f64> b = src_col(in, 2);
    std::span<atx::f64> out = dst_col(in);
    for (atx::usize i = 0; i < cells_; ++i) {
      out[i] = detail::op_select(c[i], a[i], b[i]);
    }
    return atx::core::Ok();
  }

  // ---- multi-output (Split2) -----------------------------------------------
  // Synthetic test op: hi = x, lo = -x. Occupies a contiguous two-slot block
  // [in.dst, in.dst+1] pre-sized by the linearizer's acquire_block(2).
  // Bit-identical to vm.hpp's eval_split2 by construction; the differential
  // test enforces the match. SAFETY: accessing in.dst+1 is within pool capacity
  // because num_slots already accounts for the block.
  [[nodiscard]] atx::core::Status eval_split2(const Instr &in) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const std::span<atx::f64> hi = pool_.column(in.dst + 0);
    const std::span<atx::f64> lo = pool_.column(in.dst + 1);
    for (atx::usize i = 0; i < cells_; ++i) {
      hi[i] = x[i];
      lo[i] = -x[i];
    }
    return atx::core::Ok();
  }

  // ---- cross-sectional (per date-row) -------------------------------------
  [[nodiscard]] atx::core::Status eval_cross_section(const Instr &in);

  // ---- time-series (per instrument column) --------------------------------
  [[nodiscard]] atx::core::Status eval_time_series(const Instr &in);

  // ---- stateful recurrence (forward scan, true cross-date state) -----------
  [[nodiscard]] atx::core::Status eval_recurrence(const Instr &in);
  // Per-op filter helpers — each restates the recurrence math INLINE (no shared
  // state_ops kernels), so the differential proves VM-vs-oracle independently.
  [[nodiscard]] atx::core::Status eval_kalman_level(const Instr &in, std::span<atx::f64> out) const;
  [[nodiscard]] atx::core::Status eval_ou_filter(const Instr &in, std::span<atx::f64> out) const;
  // KalmanReg: Chan 2-state regression record op (3 output pins). Writes to
  // pool_.column(dst+0/1/2). Restates the 2x2 Chan recursion INLINE — does NOT
  // call state_ops::kalman_reg_step, so the differential is a real cross-check.
  [[nodiscard]] atx::core::Status eval_kalman_reg(const Instr &in);

  // Helpers for the two big families (defined out-of-line below the class).
  void cs_one_date(OpCode op, std::span<const atx::f64> x, std::span<const atx::f64> g, atx::f64 a,
                   std::span<atx::f64> out) const;
  // Single-cell time-series kernels at (t, j) over trailing window `d`.
  [[nodiscard]] atx::f64 ts_unary_at(OpCode op, std::span<const atx::f64> x, atx::usize t,
                                     atx::usize j, atx::usize d) const;
  [[nodiscard]] atx::f64 ts_binary_at(OpCode op, std::span<const atx::f64> x,
                                      std::span<const atx::f64> y, atx::usize t, atx::usize j,
                                      atx::usize d) const;

  const Program &prog_;
  const Panel &panel_;
  atx::usize dates_{};
  atx::usize instruments_{};
  atx::usize cells_{};
  // Fully-qualified: detail also hosts bytecode.hpp's linearizer SlotPool — we
  // want the columnar-buffer pool defined in panel.hpp (the enclosing namespace).
  ::atx::engine::alpha::SlotPool pool_;
};

// =========================================================================
//  Cross-sectional kernels — per date-row over the valid set.
// =========================================================================

// Ordinal percentile rank in [0,1] over `valid` indices, tie-broken by ascending
// instrument index (NaNs already excluded). Rank r (0-based) of n maps to
// r/(n-1); a singleton set maps to 0.5 (centred — avoids a degenerate 0/0).
inline void cs_rank(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                    std::span<atx::f64> out) {
  const atx::usize n = valid.size();
  if (n == 0) {
    return;
  }
  std::vector<atx::usize> order = valid;
  // Stable sort by value, ties by instrument index (order already ascending in
  // index, std::stable_sort preserves it) -> deterministic ordinal tie-break.
  std::stable_sort(order.begin(), order.end(),
                   [&](atx::usize i, atx::usize j) { return x[i] < x[j]; });
  for (atx::usize r = 0; r < n; ++r) {
    const atx::f64 pct = (n == 1) ? 0.5 : static_cast<atx::f64>(r) / static_cast<atx::f64>(n - 1);
    out[order[r]] = pct;
  }
}

// Gather the valid-set values of `x` into a dense vector (drops invalid cells).
[[nodiscard]] inline std::vector<atx::f64> gather(std::span<const atx::f64> x,
                                                  const std::vector<atx::usize> &valid) {
  std::vector<atx::f64> v;
  v.reserve(valid.size());
  for (const atx::usize i : valid) {
    v.push_back(x[i]);
  }
  return v;
}

// CsZscore: (x - mean) / sample-std over the valid set; out-of-set -> NaN. With
// fewer than 2 valid observations the std is undefined -> all NaN.
inline void cs_zscore(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                      std::span<atx::f64> out) {
  const std::vector<atx::f64> v = gather(x, valid);
  const atx::f64 mean = mean_of(v);
  const atx::f64 sd = sample_std(v, mean);
  for (const atx::usize i : valid) {
    out[i] = (x[i] - mean) / sd; // sd NaN -> NaN; propagates correctly
  }
}

// CsScale: rescale so the sum of absolute values over the valid set equals `a`.
// A zero L1 norm (all-zero valid set) leaves the row at 0 (a/0 would be inf).
inline void cs_scale(std::span<const atx::f64> x, const std::vector<atx::usize> &valid, atx::f64 a,
                     std::span<atx::f64> out) {
  atx::f64 l1 = 0.0;
  for (const atx::usize i : valid) {
    l1 += std::fabs(x[i]);
  }
  const atx::f64 k = (l1 == 0.0) ? 0.0 : a / l1;
  for (const atx::usize i : valid) {
    out[i] = x[i] * k;
  }
}

// CsDemeanG / CsNeutG: subtract the per-group mean within the valid set.
inline void cs_group_demean(std::span<const atx::f64> x, std::span<const atx::f64> g,
                            const std::vector<atx::usize> &valid, std::span<atx::f64> out) {
  for (const atx::usize i : valid) {
    if (is_nan(g[i])) {
      continue; // no group label -> stays NaN (out-of-set)
    }
    atx::f64 sum = 0.0;
    atx::usize cnt = 0;
    for (const atx::usize j : valid) {
      if (g[j] == g[i]) {
        sum += x[j];
        ++cnt;
      }
    }
    out[i] = x[i] - sum / static_cast<atx::f64>(cnt);
  }
}

// CsRankG / CsZscoreG: rank (ordinal percentile) or sample-zscore WITHIN each
// group of the valid set. `zscore` selects the variant.
inline void cs_group(std::span<const atx::f64> x, std::span<const atx::f64> g,
                     const std::vector<atx::usize> &valid, std::span<atx::f64> out, bool zscore) {
  for (const atx::usize i : valid) {
    if (is_nan(g[i])) {
      continue;
    }
    std::vector<atx::usize> members;
    for (const atx::usize j : valid) {
      if (g[j] == g[i]) {
        members.push_back(j);
      }
    }
    if (zscore) {
      const std::vector<atx::f64> v = gather(x, members);
      const atx::f64 mean = mean_of(v);
      const atx::f64 sd = sample_std(v, mean);
      out[i] = (x[i] - mean) / sd;
    } else {
      cs_rank(x, members, out); // writes ranks for the whole group (idempotent)
    }
  }
}

// CsNormalize (P3b-2): cross-sectional demean — x - mean over the valid set.
inline void cs_normalize(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                         std::span<atx::f64> out) {
  const std::vector<atx::f64> v = gather(x, valid);
  const atx::f64 mean = mean_of(v);
  for (const atx::usize i : valid) {
    out[i] = x[i] - mean;
  }
}

// CsWinsorize (P3b-2): clamp each valid cell to [mean - k·σ, mean + k·σ] over
// the valid set; σ = SAMPLE std (ddof=1). Fewer than 2 valid -> σ NaN -> the
// comparisons are false -> the value passes through unclamped.
inline void cs_winsorize(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                         atx::f64 k, std::span<atx::f64> out) {
  const std::vector<atx::f64> v = gather(x, valid);
  const atx::f64 mean = mean_of(v);
  const atx::f64 sd = sample_std(v, mean);
  const atx::f64 lo = mean - k * sd;
  const atx::f64 hi = mean + k * sd;
  for (const atx::usize i : valid) {
    const atx::f64 xv = x[i];
    out[i] = (xv < lo) ? lo : (xv > hi ? hi : xv);
  }
}

// CsCountG / CsMeanG (P3b-2): broadcast the within-group member count or mean to
// each valid member. A NaN group label -> stays NaN (out-of-set).
inline void cs_group_count_mean(std::span<const atx::f64> x, std::span<const atx::f64> g,
                                const std::vector<atx::usize> &valid, std::span<atx::f64> out,
                                bool want_mean) {
  for (const atx::usize i : valid) {
    if (is_nan(g[i])) {
      continue;
    }
    atx::f64 sum = 0.0;
    atx::usize cnt = 0;
    for (const atx::usize j : valid) {
      if (g[j] == g[i]) {
        sum += x[j];
        ++cnt;
      }
    }
    out[i] = want_mean ? sum / static_cast<atx::f64>(cnt) : static_cast<atx::f64>(cnt);
  }
}

// CsScaleG (P3b-2): scale within each group so Σ|x| over the group's valid
// members == 1 (zero-L1 group -> 0). A NaN group label -> stays NaN.
inline void cs_group_scale(std::span<const atx::f64> x, std::span<const atx::f64> g,
                           const std::vector<atx::usize> &valid, std::span<atx::f64> out) {
  for (const atx::usize i : valid) {
    if (is_nan(g[i])) {
      continue;
    }
    atx::f64 l1 = 0.0;
    for (const atx::usize j : valid) {
      if (g[j] == g[i]) {
        l1 += std::fabs(x[j]);
      }
    }
    const atx::f64 kfac = (l1 == 0.0) ? 0.0 : 1.0 / l1;
    out[i] = x[i] * kfac;
  }
}

inline atx::core::Status Oracle::eval_cross_section(const Instr &in) {
  const std::span<const atx::f64> x = src_col(in, 0);
  std::span<atx::f64> out = dst_col(in);
  // Group ops take the classifier in src[1]; CsScale/CsWinsorize read a scalar
  // (target L1 norm / std multiplier) from src[1]'s [0] cell.
  const bool grouped =
      (in.op == OpCode::CsDemeanG || in.op == OpCode::CsNeutG || in.op == OpCode::CsRankG ||
       in.op == OpCode::CsZscoreG || in.op == OpCode::CsCountG || in.op == OpCode::CsMeanG ||
       in.op == OpCode::CsScaleG);
  std::span<const atx::f64> g{};
  atx::f64 scale_a = 1.0;
  if (grouped) {
    g = src_col(in, 1);
  } else if (in.op == OpCode::CsScale || in.op == OpCode::CsWinsorize) {
    scale_a = detail::scalar_of(src_col(in, 1));
  }
  for (atx::usize d = 0; d < dates_; ++d) {
    const std::span<const atx::f64> xr = x.subspan(d * instruments_, instruments_);
    const std::span<atx::f64> orow = out.subspan(d * instruments_, instruments_);
    const std::span<const atx::f64> grow =
        grouped ? g.subspan(d * instruments_, instruments_) : std::span<const atx::f64>{};
    cs_one_date(in.op, xr, grow, scale_a, orow);
  }
  return atx::core::Ok();
}

// cs_one_date — apply one cross-sectional op to a single date's row. `x` is the
// row's values, `g` the group labels (empty for ungrouped ops), `scale_a` the
// CsScale factor. `out` starts as all-NaN (run() pre-fills the SignalSet, but
// scratch slots are reused, so we MUST write every cell — invalid cells -> NaN).
inline void Oracle::cs_one_date(OpCode op, std::span<const atx::f64> x, std::span<const atx::f64> g,
                                atx::f64 scale_a, std::span<atx::f64> out) const {
  // The valid set: in-universe at this date AND non-NaN. The Panel universe was
  // already folded into NaN by LoadField, so "non-NaN" captures both here.
  std::vector<atx::usize> valid;
  valid.reserve(x.size());
  for (atx::usize i = 0; i < x.size(); ++i) {
    out[i] = detail::kNaN; // default every cell (out-of-set stays NaN)
    if (!detail::is_nan(x[i])) {
      valid.push_back(i);
    }
  }
  switch (op) {
  case OpCode::CsRank:
    cs_rank(x, valid, out);
    return;
  case OpCode::CsZscore:
    cs_zscore(x, valid, out);
    return;
  case OpCode::CsScale:
    cs_scale(x, valid, scale_a, out);
    return;
  case OpCode::CsNormalize:
    cs_normalize(x, valid, out);
    return;
  case OpCode::CsWinsorize:
    cs_winsorize(x, valid, scale_a, out);
    return;
  case OpCode::CsDemeanG:
  case OpCode::CsNeutG: // SAFETY: residualize-on-group-dummies == per-group demean
    cs_group_demean(x, g, valid, out);
    return;
  case OpCode::CsRankG:
    cs_group(x, g, valid, out, /*zscore=*/false);
    return;
  case OpCode::CsZscoreG:
    cs_group(x, g, valid, out, /*zscore=*/true);
    return;
  case OpCode::CsCountG:
    cs_group_count_mean(x, g, valid, out, /*want_mean=*/false);
    return;
  case OpCode::CsMeanG:
    cs_group_count_mean(x, g, valid, out, /*want_mean=*/true);
    return;
  case OpCode::CsScaleG:
    cs_group_scale(x, g, valid, out);
    return;
  default:
    ATX_UNREACHABLE();
  }
}

// =========================================================================
//  Time-series kernels — per instrument column, causal trailing window.
// =========================================================================

inline atx::core::Status Oracle::eval_time_series(const Instr &in) {
  const std::span<const atx::f64> x = src_col(in, 0);
  std::span<atx::f64> out = dst_col(in);
  // Window from the LAST operand (delay/delta/unary-window: src[1]; corr/cov:
  // src[2]). Find the highest populated operand slot.
  atx::usize last = 0;
  for (atx::usize k = 0; k < in.src.size(); ++k) {
    if (in.src.at(k) != kNoSlot) {
      last = k;
    }
  }
  const atx::usize d = detail::window_of(src_col(in, last));
  // Binary-series ops (corr/cov) read a second series from src[1].
  const bool binary_series = (in.op == OpCode::TsCorr || in.op == OpCode::TsCov);
  const std::span<const atx::f64> y = binary_series ? src_col(in, 1) : std::span<const atx::f64>{};

  for (atx::usize j = 0; j < instruments_; ++j) {
    for (atx::usize t = 0; t < dates_; ++t) {
      out[t * instruments_ + j] =
          binary_series ? ts_binary_at(in.op, x, y, t, j, d) : ts_unary_at(in.op, x, t, j, d);
    }
  }
  return atx::core::Ok();
}

// ---------------------------------------------------------------------------
//  Window gather + scalar statistics (shared by the Ts unary kernels).
// ---------------------------------------------------------------------------

// Collect the trailing window x[t-d+1 .. t] for instrument column `j` into
// `win` (chronological order). Returns false if the window is incomplete
// (t+1 < d) OR any cell is NaN -> the uniform "any-NaN/short window -> NaN"
// policy. Empty `win` on false.
[[nodiscard]] inline bool gather_window(std::span<const atx::f64> x, atx::usize t, atx::usize j,
                                        atx::usize d, atx::usize instruments,
                                        std::vector<atx::f64> &win) {
  win.clear();
  if (d == 0 || t + 1 < d) {
    return false;
  }
  for (atx::usize s = t + 1 - d; s <= t; ++s) {
    const atx::f64 v = x[s * instruments + j];
    if (is_nan(v)) {
      return false;
    }
    win.push_back(v);
  }
  return true;
}

[[nodiscard]] inline atx::f64 sum_of(const std::vector<atx::f64> &w) noexcept {
  atx::f64 s = 0.0;
  for (const atx::f64 v : w) {
    s += v;
  }
  return s;
}

[[nodiscard]] inline atx::f64 sample_var(const std::vector<atx::f64> &w) noexcept {
  if (w.size() < 2) {
    return kNaN;
  }
  const atx::f64 m = sum_of(w) / static_cast<atx::f64>(w.size());
  atx::f64 ss = 0.0;
  for (const atx::f64 v : w) {
    ss += (v - m) * (v - m);
  }
  return ss / static_cast<atx::f64>(w.size() - 1);
}

// Pearson product-moment correlation of two equal-length windows (population
// cross-moments cancel n, so corr in [-1,1]); NaN if either has zero variance.
[[nodiscard]] inline atx::f64 pearson(const std::vector<atx::f64> &a,
                                      const std::vector<atx::f64> &b) noexcept {
  const atx::usize n = a.size();
  if (n < 2) {
    return kNaN;
  }
  const atx::f64 ma = sum_of(a) / static_cast<atx::f64>(n);
  const atx::f64 mb = sum_of(b) / static_cast<atx::f64>(n);
  atx::f64 sab = 0.0;
  atx::f64 saa = 0.0;
  atx::f64 sbb = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    sab += (a[i] - ma) * (b[i] - mb);
    saa += (a[i] - ma) * (a[i] - ma);
    sbb += (b[i] - mb) * (b[i] - mb);
  }
  const atx::f64 denom = std::sqrt(saa * sbb);
  return denom == 0.0 ? kNaN : sab / denom;
}

// Sample covariance (ddof=1) of two equal-length windows.
[[nodiscard]] inline atx::f64 sample_cov(const std::vector<atx::f64> &a,
                                         const std::vector<atx::f64> &b) noexcept {
  const atx::usize n = a.size();
  if (n < 2) {
    return kNaN;
  }
  const atx::f64 ma = sum_of(a) / static_cast<atx::f64>(n);
  const atx::f64 mb = sum_of(b) / static_cast<atx::f64>(n);
  atx::f64 s = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    s += (a[i] - ma) * (b[i] - mb);
  }
  return s / static_cast<atx::f64>(n - 1);
}

// Trailing rolling-regression of the window on time (x-axis 0..n-1). Returns
// {slope, intercept, r2, fitted-at-last}. NaN slope if zero time-variance.
struct LinFit {
  atx::f64 slope{kNaN};
  atx::f64 intercept{kNaN};
  atx::f64 r2{kNaN};
  atx::f64 fitted_last{kNaN};
};
[[nodiscard]] inline LinFit lin_fit(const std::vector<atx::f64> &y) noexcept {
  const atx::usize n = y.size();
  LinFit f;
  if (n < 2) {
    return f;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  atx::f64 sx = 0.0;
  atx::f64 sy = 0.0;
  atx::f64 sxx = 0.0;
  atx::f64 sxy = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 xi = static_cast<atx::f64>(i);
    sx += xi;
    sy += y[i];
    sxx += xi * xi;
    sxy += xi * y[i];
  }
  const atx::f64 denom = nf * sxx - sx * sx;
  if (denom == 0.0) {
    return f;
  }
  f.slope = (nf * sxy - sx * sy) / denom;
  f.intercept = (sy - f.slope * sx) / nf;
  const atx::f64 my = sy / nf;
  atx::f64 ss_tot = 0.0;
  atx::f64 ss_res = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 fit = f.intercept + f.slope * static_cast<atx::f64>(i);
    ss_tot += (y[i] - my) * (y[i] - my);
    ss_res += (y[i] - fit) * (y[i] - fit);
  }
  f.r2 = (ss_tot == 0.0) ? kNaN : 1.0 - ss_res / ss_tot;
  f.fitted_last = f.intercept + f.slope * static_cast<atx::f64>(n - 1);
  return f;
}

// 1-based position of the FIRST max in the window (chronological); Alpha101's
// ts_argmax convention. Symmetric helper for argmin.
[[nodiscard]] inline atx::f64 arg_extreme(const std::vector<atx::f64> &w, bool want_max) noexcept {
  atx::usize best = 0;
  for (atx::usize i = 1; i < w.size(); ++i) {
    if ((want_max && w[i] > w[best]) || (!want_max && w[i] < w[best])) {
      best = i;
    }
  }
  return static_cast<atx::f64>(best + 1);
}

// ts_unary_at — single-cell value of a unary-series Ts op at (t, j). delay/delta
// short-circuit (they need only the shifted observation, not a full window).
inline atx::f64 Oracle::ts_unary_at(OpCode op, std::span<const atx::f64> x, atx::usize t,
                                    atx::usize j, atx::usize d) const {
  // delay/delta: x[t-d] with min_periods==1; NaN if the shift falls off the top.
  if (op == OpCode::TsDelay || op == OpCode::TsDelta) {
    if (d == 0 || t < d) {
      return detail::kNaN;
    }
    const atx::f64 shifted = x[(t - d) * instruments_ + j];
    return op == OpCode::TsDelay ? shifted : x[t * instruments_ + j] - shifted;
  }
  // ts_backfill (P3b-2): most recent valid value in [t-d+1, t], looking PAST
  // NaNs (its own policy, NOT the any-NaN -> NaN gate). Scan newest -> oldest;
  // the `t >= i` guard keeps the walk causal and underflow-safe at date 0.
  if (op == OpCode::TsBackfill) {
    for (atx::usize i = 0; i < d && t >= i; ++i) {
      const atx::f64 v = x[(t - i) * instruments_ + j];
      if (!detail::is_nan(v)) {
        return v;
      }
    }
    return detail::kNaN;
  }
  // ts_count_nans (P3b-2): full-window-only NaN count (returns a finite count,
  // never propagates NaN). An incomplete window (t+1 < d) -> NaN, like delay.
  if (op == OpCode::TsCountNans) {
    if (d == 0 || t + 1 < d) {
      return detail::kNaN;
    }
    atx::usize cnt = 0;
    for (atx::usize s = t + 1 - d; s <= t; ++s) {
      if (detail::is_nan(x[s * instruments_ + j])) {
        ++cnt;
      }
    }
    return static_cast<atx::f64>(cnt);
  }

  std::vector<atx::f64> w;
  if (!detail::gather_window(x, t, j, d, instruments_, w)) {
    return detail::kNaN; // short window or any-NaN -> NaN (pinned policy)
  }
  const atx::usize n = w.size();
  // The min_periods policy is the single authority for "enough observations".
  // gather_window already enforces a full, NaN-free window, so for the current
  // full-window policy this is a defensive no-op; it keeps the policy live so a
  // future partial-window op (or an OpSig min_periods field) routes through here.
  if (n < detail::min_periods(op, d)) {
    return detail::kNaN;
  }
  switch (op) {
  case OpCode::TsSum:
    return detail::sum_of(w);
  case OpCode::TsMean:
    return detail::sum_of(w) / static_cast<atx::f64>(n);
  case OpCode::TsVar:
    return detail::sample_var(w);
  case OpCode::TsStd:
    return std::sqrt(detail::sample_var(w));
  case OpCode::TsMin:
    return *std::min_element(w.begin(), w.end());
  case OpCode::TsMax:
    return *std::max_element(w.begin(), w.end());
  case OpCode::TsArgMin:
    return detail::arg_extreme(w, /*want_max=*/false);
  case OpCode::TsArgMax:
    return detail::arg_extreme(w, /*want_max=*/true);
  case OpCode::TsProduct: {
    atx::f64 p = 1.0;
    for (const atx::f64 v : w) {
      p *= v;
    }
    return p;
  }
  case OpCode::TsRank: {
    // Ordinal percentile rank of the LAST element within its window, [0,1].
    atx::usize less = 0;
    atx::usize equal = 0;
    for (const atx::f64 v : w) {
      if (v < w.back()) {
        ++less;
      } else if (v == w.back()) {
        ++equal;
      }
    }
    // Average-rank for ties, then normalize to [0,1] over n elements.
    const atx::f64 avg = static_cast<atx::f64>(less) + (static_cast<atx::f64>(equal) - 1.0) / 2.0;
    return n == 1 ? 0.5 : avg / static_cast<atx::f64>(n - 1);
  }
  case OpCode::TsMed: {
    std::vector<atx::f64> s = w;
    std::sort(s.begin(), s.end());
    return (n % 2 == 1) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2.0;
  }
  case OpCode::TsMad: {
    const atx::f64 m = detail::sum_of(w) / static_cast<atx::f64>(n);
    atx::f64 s = 0.0;
    for (const atx::f64 v : w) {
      s += std::fabs(v - m);
    }
    return s / static_cast<atx::f64>(n);
  }
  case OpCode::TsDecayLinear: {
    // Linear weights d, d-1, .. 1 (newest heaviest), normalized to sum 1.
    atx::f64 acc = 0.0;
    atx::f64 wsum = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 wt = static_cast<atx::f64>(i + 1); // oldest=1 .. newest=n
      acc += wt * w[i];
      wsum += wt;
    }
    return acc / wsum;
  }
  case OpCode::TsWma: {
    // Weighted MA, same linear weights as decay_linear (alias here).
    atx::f64 acc = 0.0;
    atx::f64 wsum = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 wt = static_cast<atx::f64>(i + 1);
      acc += wt * w[i];
      wsum += wt;
    }
    return acc / wsum;
  }
  case OpCode::TsEma: {
    // Exponential MA over the window, alpha = 2/(d+1), seeded with the oldest.
    const atx::f64 alpha = 2.0 / (static_cast<atx::f64>(n) + 1.0);
    atx::f64 ema = w.front();
    for (atx::usize i = 1; i < n; ++i) {
      ema = alpha * w[i] + (1.0 - alpha) * ema;
    }
    return ema;
  }
  case OpCode::TsSkew: {
    if (n < 3) {
      return detail::kNaN;
    }
    const atx::f64 m = detail::sum_of(w) / static_cast<atx::f64>(n);
    const atx::f64 sd = std::sqrt(detail::sample_var(w));
    if (sd == 0.0) {
      return detail::kNaN;
    }
    atx::f64 s = 0.0;
    for (const atx::f64 v : w) {
      s += (v - m) * (v - m) * (v - m);
    }
    return s / static_cast<atx::f64>(n) / (sd * sd * sd);
  }
  case OpCode::TsKurt: {
    if (n < 4) {
      return detail::kNaN;
    }
    const atx::f64 m = detail::sum_of(w) / static_cast<atx::f64>(n);
    const atx::f64 var = detail::sample_var(w);
    if (var == 0.0) {
      return detail::kNaN;
    }
    atx::f64 s = 0.0;
    for (const atx::f64 v : w) {
      s += (v - m) * (v - m) * (v - m) * (v - m);
    }
    return s / static_cast<atx::f64>(n) / (var * var) - 3.0; // excess kurtosis
  }
  case OpCode::TsSlope:
    return detail::lin_fit(w).slope;
  case OpCode::TsRsquare:
    return detail::lin_fit(w).r2;
  case OpCode::TsResid:
    return w.back() - detail::lin_fit(w).fitted_last;
  case OpCode::TsZscore: {
    // (x[t] - mean) / sample-std over the window; same reductions as mean/std.
    const atx::f64 mean = detail::sum_of(w) / static_cast<atx::f64>(n);
    const atx::f64 sd = std::sqrt(detail::sample_var(w));
    return (w.back() - mean) / sd; // sd NaN (n<2) -> NaN
  }
  case OpCode::TsAvDiff:
    return w.back() - detail::sum_of(w) / static_cast<atx::f64>(n);
  case OpCode::TsQuantile: {
    // Rolling median (quantile 0.5) — identical to TsMed (sort + even-n midpoint).
    std::vector<atx::f64> s = w;
    std::sort(s.begin(), s.end());
    return (n % 2 == 1) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2.0;
  }
  case OpCode::TsScale: {
    // Rolling min-max: (x[t] - min) / (max - min); flat window -> 0.
    const atx::f64 lo = *std::min_element(w.begin(), w.end());
    const atx::f64 hi = *std::max_element(w.begin(), w.end());
    const atx::f64 range = hi - lo;
    return range == 0.0 ? 0.0 : (w.back() - lo) / range;
  }
  default:
    ATX_UNREACHABLE();
  }
}

// ts_binary_at — corr/cov of the trailing windows of x and y at (t, j).
inline atx::f64 Oracle::ts_binary_at(OpCode op, std::span<const atx::f64> x,
                                     std::span<const atx::f64> y, atx::usize t, atx::usize j,
                                     atx::usize d) const {
  std::vector<atx::f64> wx;
  std::vector<atx::f64> wy;
  if (!detail::gather_window(x, t, j, d, instruments_, wx) ||
      !detail::gather_window(y, t, j, d, instruments_, wy)) {
    return detail::kNaN;
  }
  return op == OpCode::TsCorr ? detail::pearson(wx, wy) : detail::sample_cov(wx, wy);
}

// =========================================================================
//  Stateful causal recurrences — INDEPENDENT reference (P3b-3).
//
//  trade_when / hump carry TRUE cross-date state from the panel's first date
//  forward (no trailing window), so they do NOT route through eval_time_series.
//  This is the obviously-correct REFERENCE the fast VM (vm.hpp::eval_recurrence)
//  must reproduce BIT-FOR-BIT; the branch order and NaN policy below are PINNED
//  and re-stated independently of state_ops.hpp (the differential test proves
//  the two agree). Per instrument column j we walk dates t=0…D-1 forward,
//  carrying the prior output in a scalar `prior`.
//
//  SAFETY: causal by construction. The inner step reads only `prior`
//  (== out[t-1,j]) and the date-t operand cells at index t*I+j; there is no
//  index into a future date or future state. The first date (t==0) is special-
//  cased, seeding `prior` without any look-back. No std container grows in the
//  scan (the SignalSet/slot buffers are pre-sized), so this allocates nothing.
// =========================================================================
// KalmanLevel oracle reference — scalar local-level Kalman filter, per-instrument
// forward scan. Hyperparams Q (process noise) and R (observation noise) from
// in.imm[0/1]. The recurrence math is restated INLINE here (NOT via the shared
// state_ops kernels the VM uses), so the differential proves the two paths agree
// independently. Stack-local {x, P, seeded} per instrument — no shared buffer.
// SAFETY: causal by construction (reads only prior state + date-t input).
inline atx::core::Status Oracle::eval_kalman_level(const Instr &in, std::span<atx::f64> out) const {
  const std::span<const atx::f64> z = src_col(in, 0);
  const atx::f64 Q = in.imm[0];
  const atx::f64 R = in.imm[1];
  for (atx::usize j = 0; j < instruments_; ++j) {
    atx::f64 x = 0.0;
    atx::f64 P = 0.0;
    bool seeded = false;
    for (atx::usize t = 0; t < dates_; ++t) {
      const atx::usize idx = t * instruments_ + j;
      const atx::f64 zv = z[idx];
      if (!seeded) {
        if (detail::is_nan(zv)) {
          out[idx] = detail::kNaN; // unseeded NaN -> NaN, stay unseeded
          continue;
        }
        x = zv; // seed: x=z, P=R
        P = R;
        seeded = true;
        out[idx] = x;
        continue;
      }
      P += Q; // predict
      if (!detail::is_nan(zv)) {
        const atx::f64 K = P / (P + R);
        x += K * (zv - x);
        P = (1.0 - K) * P;
      }
      out[idx] = x;
    }
  }
  return atx::core::Ok();
}

// OuFilter oracle reference — OU AR(1) pull-to-mean smoother, per-instrument
// forward scan. Hyperparams theta (mean-reversion) and mu (long-run mean) from
// in.imm[0/1]. Restated INLINE (NOT the shared state_ops kernel). The pull is
// OBSERVATION-FREE after seeding: xhat = mu + phi*(xhat - mu), phi = exp(-theta);
// the new x[t] is intentionally ignored once seeded (spec §4.3). Stack-local
// {xhat, seeded} per instrument. SAFETY: causal (reads only prior xhat + date-t x).
inline atx::core::Status Oracle::eval_ou_filter(const Instr &in, std::span<atx::f64> out) const {
  const std::span<const atx::f64> x = src_col(in, 0);
  const atx::f64 theta = in.imm[0];
  const atx::f64 mu = in.imm[1];
  for (atx::usize j = 0; j < instruments_; ++j) {
    atx::f64 xhat = 0.0;
    bool seeded = false;
    for (atx::usize t = 0; t < dates_; ++t) {
      const atx::usize idx = t * instruments_ + j;
      const atx::f64 xv = x[idx];
      if (!seeded) {
        if (detail::is_nan(xv)) {
          out[idx] = detail::kNaN; // unseeded NaN -> NaN, stay unseeded
          continue;
        }
        xhat = xv; // seed
        seeded = true;
        out[idx] = xhat;
        continue;
      }
      const atx::f64 phi = std::exp(-theta);
      xhat = mu + phi * (xhat - mu); // observation-free pull toward mu
      out[idx] = xhat;
    }
  }
  return atx::core::Ok();
}

// KalmanReg oracle reference — Chan 2-state time-varying regression of y on x,
// per-instrument forward scan. Hyperparams delta and R from in.imm[0/1].
// Writes three contiguous output columns: alpha=pool_.column(dst+0),
// beta=pool_.column(dst+1), resid=pool_.column(dst+2). The 2x2 Chan recursion
// is restated INLINE here — does NOT call state_ops::kalman_reg_step — so the
// VM-vs-oracle differential is an independent cross-check.
// Diffuse prior: a=b=0, P00=P11=1, P01=0 (matches KalmanRegState defaults).
// Incomplete obs (y or x NaN): predict-only (P00+=w, P11+=w), output NaN.
// SAFETY: causal by construction (reads only prior state + date-t inputs).
inline atx::core::Status Oracle::eval_kalman_reg(const Instr &in) {
  const std::span<const atx::f64> yv = src_col(in, 0);
  const std::span<const atx::f64> xv = src_col(in, 1);
  const atx::f64 delta = in.imm[0];
  const atx::f64 R = in.imm[1];
  const std::span<atx::f64> oa = pool_.column(in.dst + 0);
  const std::span<atx::f64> ob = pool_.column(in.dst + 1);
  const std::span<atx::f64> orr = pool_.column(in.dst + 2);
  for (atx::usize j = 0; j < instruments_; ++j) {
    atx::f64 a = 0.0;
    atx::f64 b = 0.0;
    atx::f64 P00 = 1.0;
    atx::f64 P01 = 0.0;
    atx::f64 P11 = 1.0;
    for (atx::usize t = 0; t < dates_; ++t) {
      const atx::usize i = t * instruments_ + j;
      const atx::f64 y = yv[i];
      const atx::f64 x = xv[i];
      const atx::f64 w = delta / (1.0 - delta);
      if (detail::is_nan(y) || detail::is_nan(x)) {
        P00 += w;
        P11 += w;
        oa[i] = detail::kNaN;
        ob[i] = detail::kNaN;
        orr[i] = detail::kNaN;
        continue;
      }
      const atx::f64 P00p = P00 + w;
      const atx::f64 P01p = P01;
      const atx::f64 P11p = P11 + w;
      const atx::f64 e = y - (a + b * x);
      const atx::f64 pf0 = P00p + P01p * x;
      const atx::f64 pf1 = P01p + P11p * x;
      const atx::f64 Qv = pf0 + x * pf1 + R;
      const atx::f64 k0 = pf0 / Qv;
      const atx::f64 k1 = pf1 / Qv;
      a += k0 * e;
      b += k1 * e;
      P00 = P00p - k0 * pf0;
      P01 = P01p - k0 * pf1;
      P11 = P11p - k1 * pf1;
      oa[i] = a;
      ob[i] = b;
      orr[i] = e / std::sqrt(Qv);
    }
  }
  return atx::core::Ok();
}

inline atx::core::Status Oracle::eval_recurrence(const Instr &in) {
  std::span<atx::f64> out = dst_col(in);
  if (in.op == OpCode::KalmanLevel) {
    return eval_kalman_level(in, out);
  }
  if (in.op == OpCode::OuFilter) {
    return eval_ou_filter(in, out);
  }
  if (in.op == OpCode::Hump) {
    const std::span<const atx::f64> x = src_col(in, 0);
    // Scalar threshold from the 2nd operand's [0] cell (read like CsScale's `a`);
    // absent optional -> the 0.01 default (P3b-1 default-fill usually supplies it).
    const atx::f64 thr =
        in.src.at(1) == kNoSlot ? atx::f64{0.01} : detail::scalar_of(src_col(in, 1));
    for (atx::usize j = 0; j < instruments_; ++j) {
      atx::f64 prior = detail::kNaN;
      for (atx::usize t = 0; t < dates_; ++t) {
        const atx::usize i = t * instruments_ + j;
        // first date -> x[0]; else pass x[t] iff |x[t]-prior| STRICTLY > thr,
        // holding the prior otherwise (a NaN diff is never > thr -> holds).
        const atx::f64 v = (t == 0) ? x[i] : (std::fabs(x[i] - prior) > thr ? x[i] : prior);
        out[i] = v;
        prior = v;
      }
    }
    return atx::core::Ok();
  }
  // TradeWhen: trigger=src[0], alpha=src[1], exit=src[2]. Exit checked FIRST.
  const std::span<const atx::f64> trig = src_col(in, 0);
  const std::span<const atx::f64> alpha = src_col(in, 1);
  const std::span<const atx::f64> exit_v = src_col(in, 2);
  for (atx::usize j = 0; j < instruments_; ++j) {
    atx::f64 prior = detail::kNaN;
    for (atx::usize t = 0; t < dates_; ++t) {
      const atx::usize i = t * instruments_ + j;
      atx::f64 v;
      if (detail::mask_true(exit_v[i])) {
        v = detail::kNaN; // close / no position
      } else if (detail::mask_true(trig[i])) {
        v = alpha[i]; // (re)enter with the new signal
      } else {
        v = (t == 0) ? detail::kNaN : prior; // hold (flat on the first date)
      }
      out[i] = v;
      prior = v;
    }
  }
  return atx::core::Ok();
}

} // namespace detail
} // namespace atx::engine::alpha

// =========================================================================
//  evaluate_reference — the public entry point.
// =========================================================================

namespace atx::engine::alpha {

// Execute `prog` over `panel`, returning one alpha per Program root. The Program
// MUST have been compiled (its field ids index `panel`'s field dictionary by
// the SAME names via `prog.fields`). Err(InvalidArgument) if a referenced field
// is absent from the Panel; Err(Internal) never (the linearizer's invariants
// hold). Borrows both inputs by const ref; allocates a fresh SlotPool per call.
[[nodiscard]] inline atx::core::Result<SignalSet> evaluate_reference(const Program &prog,
                                                                     const Panel &panel) {
  // Validate every LoadField references a field present in the Panel, mapping
  // the Program's field-dictionary id to the Panel's field id IN PLACE is not
  // possible (Program is const); instead we pre-resolve and rewrite a local copy
  // of LoadField params. Simpler: verify names match and ids align.
  // The linearizer stores field NAMES in prog.fields, indexed by the param id.
  // We require the Panel to expose those same fields; resolve each used field.
  for (const Instr &in : prog.code) {
    if (in.op == OpCode::LoadField) {
      if (in.param >= prog.fields.size()) {
        return atx::core::Err(atx::core::ErrorCode::Internal,
                              "evaluate_reference: LoadField param out of field-dictionary range");
      }
      ATX_TRY(const FieldId pid, panel.field_id(prog.fields[in.param]));
      ATX_UNUSED(pid);
    }
  }

  // Build a Program copy whose LoadField params index the PANEL's fields (the
  // dictionaries may differ in order). This keeps eval_load_field a direct
  // field_all() lookup with no per-cell name resolution.
  Program local = prog;
  for (Instr &in : local.code) {
    if (in.op == OpCode::LoadField) {
      ATX_TRY(const FieldId pid, panel.field_id(prog.fields[in.param]));
      in.param = pid;
    }
  }

  detail::Oracle oracle{local, panel};
  return oracle.run();
}

} // namespace atx::engine::alpha
