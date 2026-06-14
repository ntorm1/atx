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
    case OpCode::CsResidualize:
    case OpCode::CsQuantile:
    case OpCode::CsVecSum:
    case OpCode::CsVecAvg:
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
    // BRAIN-superset rolling ops (S3.2): same windowed path.
    case OpCode::TsRegression:
    case OpCode::TsDecayExp:
    case OpCode::TsEntropy:
    case OpCode::TsMoment:
    // OU rolling-fit ops (P3d-E3): same windowed path as Ts* rolling ops.
    case OpCode::OuTheta:
    case OpCode::OuHalflife:
    case OpCode::OuMean:
    case OpCode::OuZscore:
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
  void cs_one_date(OpCode op, std::span<const atx::f64> x, std::span<const atx::f64> g,
                   std::span<const atx::f64> z, atx::f64 a, std::span<atx::f64> out) const;
  // Single-cell time-series kernels at (t, j) over trailing window `d`. `p0` is
  // the peeled hparam immediate (decay factor / moment order / entropy buckets);
  // 0 for ops that carry none.
  [[nodiscard]] atx::f64 ts_unary_at(OpCode op, std::span<const atx::f64> x, atx::usize t,
                                     atx::usize j, atx::usize d, atx::f64 p0) const;
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
void cs_rank(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
             std::span<atx::f64> out);

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
void cs_zscore(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
               std::span<atx::f64> out);

// CsScale: rescale so the sum of absolute values over the valid set equals `a`.
// A zero L1 norm (all-zero valid set) leaves the row at 0 (a/0 would be inf).
void cs_scale(std::span<const atx::f64> x, const std::vector<atx::usize> &valid, atx::f64 a,
              std::span<atx::f64> out);

// CsDemeanG / CsNeutG: subtract the per-group mean within the valid set.
void cs_group_demean(std::span<const atx::f64> x, std::span<const atx::f64> g,
                     const std::vector<atx::usize> &valid, std::span<atx::f64> out);

// CsRankG / CsZscoreG: rank (ordinal percentile) or sample-zscore WITHIN each
// group of the valid set. `zscore` selects the variant.
void cs_group(std::span<const atx::f64> x, std::span<const atx::f64> g,
              const std::vector<atx::usize> &valid, std::span<atx::f64> out, bool zscore);

// CsQuantile (S3.3): discretize the valid set into `n` quantile buckets
// (value = bucket/(n-1), ordinal rank as cs_rank); n < 2 -> NaN. Bit-identical
// to cs_ops.hpp's cs_quantile_row.
void cs_quantile(std::span<const atx::f64> x, const std::vector<atx::usize> &valid, atx::f64 n_real,
                 std::span<atx::f64> out);

// CsVecSum / CsVecAvg (S3.3): reduce over the valid set (sum / mean) and
// broadcast the scalar back to every valid cell. `want_avg` selects the variant.
void cs_vec_reduce(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                   std::span<atx::f64> out, bool want_avg);

// CsNormalize (P3b-2): cross-sectional demean — x - mean over the valid set.
void cs_normalize(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                  std::span<atx::f64> out);

// CsWinsorize (P3b-2): clamp each valid cell to [mean - k·σ, mean + k·σ] over
// the valid set; σ = SAMPLE std (ddof=1). Fewer than 2 valid -> σ NaN -> the
// comparisons are false -> the value passes through unclamped.
void cs_winsorize(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                  atx::f64 k, std::span<atx::f64> out);

// CsCountG / CsMeanG (P3b-2): broadcast the within-group member count or mean to
// each valid member. A NaN group label -> stays NaN (out-of-set).
void cs_group_count_mean(std::span<const atx::f64> x, std::span<const atx::f64> g,
                         const std::vector<atx::usize> &valid, std::span<atx::f64> out,
                         bool want_mean);

// CsScaleG (P3b-2): scale within each group so Σ|x| over the group's valid
// members == 1 (zero-L1 group -> 0). A NaN group label -> stays NaN.
void cs_group_scale(std::span<const atx::f64> x, std::span<const atx::f64> g,
                    const std::vector<atx::usize> &valid, std::span<atx::f64> out);

// =========================================================================
//  Time-series kernels — per instrument column, causal trailing window.
// =========================================================================

// ---------------------------------------------------------------------------
//  Window gather + scalar statistics (shared by the Ts unary kernels).
// ---------------------------------------------------------------------------

// Collect the trailing window x[t-d+1 .. t] for instrument column `j` into
// `win` (chronological order). Returns false if the window is incomplete
// (t+1 < d) OR any cell is NaN -> the uniform "any-NaN/short window -> NaN"
// policy. Empty `win` on false.
[[nodiscard]] bool gather_window(std::span<const atx::f64> x, atx::usize t, atx::usize j,
                                 atx::usize d, atx::usize instruments, std::vector<atx::f64> &win);

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
[[nodiscard]] atx::f64 pearson(const std::vector<atx::f64> &a,
                               const std::vector<atx::f64> &b) noexcept;

// Sample covariance (ddof=1) of two equal-length windows.
[[nodiscard]] atx::f64 sample_cov(const std::vector<atx::f64> &a,
                                  const std::vector<atx::f64> &b) noexcept;

// Trailing rolling-regression of the window on time (x-axis 0..n-1). Returns
// {slope, intercept, r2, fitted-at-last}. NaN slope if zero time-variance.
struct LinFit {
  atx::f64 slope{kNaN};
  atx::f64 intercept{kNaN};
  atx::f64 r2{kNaN};
  atx::f64 fitted_last{kNaN};
};
[[nodiscard]] LinFit lin_fit(const std::vector<atx::f64> &y) noexcept;

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

// Trailing AR(1) OLS of the window: regress w[s] on w[s-1] over the lagged pairs
// (oldest..newest). Returns {a, b, resid_std}; NaN fields when <2 pairs or zero
// predictor variance. resid_std is the POPULATION std of the residuals over the
// n pairs. This is the INDEPENDENT oracle restatement of ts_ops `ou_ar1_fit` —
// the windowed-family differential pairs the VM's gathered kernel with THIS
// gathered-window restatement (cf. lin_fit vs tsv_lin_fit). Do NOT collapse the
// two to a shared kernel; that would defeat the cross-check. The caller has
// already enforced a full, NaN-free window (gather_window), so no NaN-skip here.
struct OuFit {
  atx::f64 a{kNaN};
  atx::f64 b{kNaN};
  atx::f64 resid_std{kNaN};
};
[[nodiscard]] OuFit ou_fit(const std::vector<atx::f64> &w) noexcept;

// OU derived-quantity per-cell value over a gathered window `w` (newest last).
// INDEPENDENT restatement of the ts_ops ou_*_of mappers:
//   theta    = -ln(b)                       valid when b in (0,1)
//   halflife = ln2 / theta                  valid when b in (0,1)
//   mean     = a / (1-b)                     valid when b < 1 (b not NaN)
//   zscore   = (w.back()-mean) / sigma_eq    sigma_eq = resid_std/sqrt(1-b^2)
[[nodiscard]] atx::f64 ou_unary_at(OpCode op, const std::vector<atx::f64> &w) noexcept;

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
[[nodiscard]] atx::core::Result<SignalSet> evaluate_reference(const Program &prog,
                                                              const Panel &panel);

} // namespace atx::engine::alpha
