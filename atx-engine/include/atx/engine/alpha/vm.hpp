#pragma once

// atx::engine::alpha — fast vectorized VM core (P3-6).
//
// `Engine::evaluate` executes a linearized `Program` over a `Panel` on the
// PRODUCTION path the P3-9 differential harness checks against the P3-5
// tree-walking oracle. It is the FAST, independent implementation: contiguous,
// SIMD-friendly per-opcode kernels and ZERO allocation in the dispatch loop. It
// MUST reproduce `evaluate_reference` BIT-FOR-BIT for every element-wise /
// logical / select program (the differential test enforces this).
//
// ===========================================================================
//  EVAL MODEL — FULL-BUFFER COLUMNAR, batch-per-opcode (NOT a date-loop-outer)
// ===========================================================================
//  Each live SlotId holds a WHOLE `dates*instruments` f64 buffer; each
//  instruction executes ONCE over the entire panel in a tight contiguous loop.
//  We deliberately deviate from the plan's `for (date t) for (instr) …`
//  cross-section sketch and adopt the oracle's full-buffer model because:
//    (1) it is the canonical vectorized-interpreter shape (DuckDB / Vectorwise
//        X100, research Appendix B — "vectorized interpretation, batch per
//        opcode"): one kernel call sweeps a contiguous column, the loop body is
//        branch-light and auto-vectorizes;
//    (2) it shares the oracle's exact data layout (date-major
//        `date*instruments + inst`), so the P3-9 differential is robust — both
//        paths index identically and any divergence is a real numeric bug, not
//        a reshape artifact;
//    (3) the per-date cross-section kernels (P3-7) and the per-instrument
//        trailing-window kernels (P3-8) plug in with the FULL panel already
//        materialized — no ring-buffered rolling state to thread through;
//    (4) peak-live-slots is small (3–5 typical; `Program::num_slots`), so the
//        `num_slots * dates * instruments` working set stays bounded.
//  The VM still earns its keep over the oracle: SIMD-friendly contiguous
//  arithmetic, zero dispatch-loop allocation, and (for P3-8) O(1)/cell rolling.
//
//  SIMD: the element-wise kernels are plain contiguous `f64*` loops left to the
//  compiler's auto-vectorizer rather than atx-core's L5 `simd::*`. Element-wise
//  ops carry NO reduction, so a lane-parallel sweep is bit-identical to the
//  oracle's scalar loop; using a single uniform loop style (a) keeps the per-op
//  scalar semantics provably identical to the oracle's `detail::op_*` lambdas
//  and (b) avoids L5's reduction-ordering caveats entirely.
//
// ===========================================================================
//  PINNED SEMANTIC CONTRACT — re-implemented here, self-contained
// ===========================================================================
//  vm.hpp does NOT include oracle.hpp; it re-states the SAME scalar policy so
//  the two paths are independent (the differential TEST proves they agree):
//    * arithmetic + - * / : raw IEEE (NaN/inf propagate naturally).
//    * Pow = std::pow; Spow = sign(x)*pow(|x|,e), NaN if either operand NaN.
//    * MinP/MaxP : NaN if EITHER operand is NaN (NOT std::min/max's pick).
//    * comparisons -> 1.0/0.0 mask, NaN if either comparand is NaN.
//    * And/Or : finite-non-zero is true, 0 false, NaN -> NaN.
//    * Not = 1-x for a 0/1 mask, NaN -> NaN.
//    * Select(c,a,b) : NaN c -> NaN, else c!=0 ? a : b.
//    * Neg / Abs / Sign (NaN->NaN, ±/0) / Log = std::log.
//    * LoadField NaNs out-of-universe cells (point-in-time). Const fills imm.
//  Cross-sectional (Cs*) and time-series (Ts*) opcodes are NOT YET implemented:
//  they return Err(NotImplemented) here and land in P3-7 / P3-8 respectively.
//
// SAFETY (topological execution): the Program's instruction stream is
// topologically ordered (the linearizer emits in DAG NodeId order; every `src`
// slot is produced by an EARLIER instr — see bytecode.hpp), so a single forward
// pass reads each slot only after the instruction that wrote it.
//
// Ownership / lifetime: the Engine BORROWS the Panel by const ref for its whole
// lifetime; it owns a reusable SlotPool (grown only when a program needs more
// slots than any prior call) plus pre-sized field-remap scratch, so a warm
// second evaluate() allocates nothing in the dispatch loop. Header-only; every
// free function is `inline`. Rule of Zero.

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
#include "atx/engine/alpha/cs_ops.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/state_ops.hpp"
#include "atx/engine/alpha/ts_ops.hpp"

namespace atx::engine::alpha {

namespace detail {

inline constexpr atx::f64 kVmNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] inline bool vm_is_nan(atx::f64 x) noexcept { return std::isnan(x); }

// A mask cell is "true" iff finite and non-zero; NaN is neither (callers handle
// NaN before consulting truth). Mirrors oracle.hpp's mask_true exactly.
[[nodiscard]] inline bool vm_mask_true(atx::f64 x) noexcept { return x != 0.0 && !vm_is_nan(x); }

// ===========================================================================
//  Scalar element-wise kernels — bit-identical to oracle.hpp's `detail::op_*`.
//  Restated here (vm.hpp is self-contained); the differential test enforces the
//  match. `noexcept` leaf math.
// ===========================================================================

[[nodiscard]] inline atx::f64 vm_min(atx::f64 a, atx::f64 b) noexcept {
  if (vm_is_nan(a) || vm_is_nan(b)) {
    return kVmNaN;
  }
  return a < b ? a : b;
}

[[nodiscard]] inline atx::f64 vm_max(atx::f64 a, atx::f64 b) noexcept {
  if (vm_is_nan(a) || vm_is_nan(b)) {
    return kVmNaN;
  }
  return a > b ? a : b;
}

[[nodiscard]] inline atx::f64 vm_sign(atx::f64 a) noexcept {
  if (vm_is_nan(a)) {
    return kVmNaN;
  }
  return static_cast<atx::f64>(a > 0.0) - static_cast<atx::f64>(a < 0.0);
}

// signedpower(x, e) = sign(x) * |x|^e (Alpha101 SignedPower).
[[nodiscard]] inline atx::f64 vm_spow(atx::f64 a, atx::f64 e) noexcept {
  if (vm_is_nan(a) || vm_is_nan(e)) {
    return kVmNaN;
  }
  return vm_sign(a) * std::pow(std::fabs(a), e);
}

[[nodiscard]] inline atx::f64 vm_and(atx::f64 a, atx::f64 b) noexcept {
  if (vm_is_nan(a) || vm_is_nan(b)) {
    return kVmNaN;
  }
  return (vm_mask_true(a) && vm_mask_true(b)) ? 1.0 : 0.0;
}

[[nodiscard]] inline atx::f64 vm_or(atx::f64 a, atx::f64 b) noexcept {
  if (vm_is_nan(a) || vm_is_nan(b)) {
    return kVmNaN;
  }
  return (vm_mask_true(a) || vm_mask_true(b)) ? 1.0 : 0.0;
}

[[nodiscard]] inline atx::f64 vm_not(atx::f64 a) noexcept {
  return vm_is_nan(a) ? kVmNaN : (vm_mask_true(a) ? 0.0 : 1.0);
}

[[nodiscard]] inline atx::f64 vm_select(atx::f64 c, atx::f64 a, atx::f64 b) noexcept {
  if (vm_is_nan(c)) {
    return kVmNaN;
  }
  return vm_mask_true(c) ? a : b;
}

// ===========================================================================
//  Contiguous map kernels — one tight loop per opcode; auto-vectorizable.
//  `out`, `a`, `b`, `c` are co-sized whole-panel buffers.
// ===========================================================================

// Distinct names from oracle.hpp's `detail::map_*`: a TU that includes BOTH
// headers (the differential test) must not collide in this shared namespace.
template <class F>
inline void vm_map_unary(std::span<const atx::f64> a, std::span<atx::f64> out, F f) noexcept {
  const atx::usize n = out.size();
  for (atx::usize i = 0; i < n; ++i) {
    out[i] = f(a[i]);
  }
}

template <class F>
inline void vm_map_binary(std::span<const atx::f64> a, std::span<const atx::f64> b,
                          std::span<atx::f64> out, F f) noexcept {
  const atx::usize n = out.size();
  for (atx::usize i = 0; i < n; ++i) {
    out[i] = f(a[i], b[i]);
  }
}

// Comparison -> mask (NaN if either operand is NaN). Mirrors oracle's map_cmp.
template <class Cmp>
inline void vm_map_cmp(std::span<const atx::f64> a, std::span<const atx::f64> b,
                       std::span<atx::f64> out, Cmp cmp) noexcept {
  const atx::usize n = out.size();
  for (atx::usize i = 0; i < n; ++i) {
    out[i] = (vm_is_nan(a[i]) || vm_is_nan(b[i])) ? kVmNaN : (cmp(a[i], b[i]) ? 1.0 : 0.0);
  }
}

} // namespace detail

// =========================================================================
//  Engine — the fast vectorized executor (the production path).
//
//  Borrows the Panel for its lifetime, owns a reusable SlotPool + field-remap
//  scratch that grow monotonically across calls so a warm evaluate() allocates
//  nothing in the dispatch loop. Decomposed into per-family `kernel_*` helpers
//  (each a contiguous `f64*` loop) so every member stays under the 60-line cap
//  and the dispatch switch stays exhaustive (no default).
// =========================================================================

class Engine {
public:
  // Borrows `panel` for the Engine's lifetime (non-owning const ref). Cheap; no
  // pool is sized until the first evaluate() (the program's num_slots is unknown
  // until then).
  explicit Engine(const Panel &panel) noexcept : panel_{panel} {}

  // Evaluate a compiled Program -> one alpha per root. Element-wise / logical /
  // select are implemented; Cs*/Ts* return Err(NotImplemented) until P3-7/P3-8.
  //
  // The SlotPool is reused across calls (grown only if a program needs more
  // slots than any prior call), so a warm second evaluate() allocates nothing in
  // the dispatch loop — only the output SignalSet buffers are fresh per call.
  //
  // Errors: Err(NotFound) if a referenced field is absent from the Panel;
  // Err(Internal) if a LoadField / StoreAlpha param is out of range;
  // Err(NotImplemented) on a Cs*/Ts* opcode.
  [[nodiscard]] atx::core::Result<SignalSet> evaluate(const Program &prog) {
    const atx::usize dates = panel_.dates();
    const atx::usize instruments = panel_.instruments();
    const atx::usize cells = dates * instruments;

    ATX_TRY_VOID(resolve_fields(prog)); // fills field_remap_ (allocates only on growth)
    ensure_pool(prog.num_slots, cells); // (re)sizes the pool only on growth

    SignalSet out;
    out.dates = dates;
    out.instruments = instruments;
    out.alphas.resize(prog.roots.size());
    for (atx::usize r = 0; r < prog.roots.size(); ++r) {
      out.alphas[r].name = prog.roots[r].name;
      out.alphas[r].values.assign(cells, detail::kVmNaN);
    }

    // ---- ZERO-ALLOC dispatch loop (everything below allocates nothing) ----
    // Program SlotIds index the pool buffer directly (the linearizer pre-sized
    // num_slots); acquire()/release() only honor the pool's live-count assert.
    // LIVENESS CONTRACT for multi-output nodes:
    //   * The linearizer's acquire_block(n_out) grew peak by n_out, so the pool
    //     buffer has n_out contiguous slots starting at in.dst.
    //   * The VM mirrors the ACQUIRE side with exactly ONE (void)pool_.acquire()
    //     per dispatched compute instr regardless of n_out. A Split2 block wrote
    //     two buffer columns but the live-count assert only needs to stay <=
    //     capacity — capacity == num_slots which already accounts for the block.
    //   * Pin is a value-producing instr (occupies its own single slot); it is
    //     handled here before dispatch (like StoreAlpha) so we can copy from
    //     the source block without routing through the generic switch.
    //   * Free calls release once regardless of n_out (mirrors the single
    //     acquire above). The block's extra buffer slots remain valid throughout
    //     execution since the flat buffer is pre-sized to num_slots total slots.
    for (const Instr &in : prog.code) {
      if (in.op == OpCode::Free) {
        pool_.release(in.dst);
        continue;
      }
      if (in.op == OpCode::StoreAlpha) {
        ATX_TRY_VOID(store_alpha(in, out, cells));
        continue;
      }
      if (in.op == OpCode::Pin) {
        // Pin projects one output of its parent's contiguous block into its own
        // single slot: column(src[0] + param) -> column(dst). One acquire for
        // the single dst slot; no dispatch needed (pure buffer copy).
        (void)pool_.acquire();
        const std::span<const atx::f64> src = pool_.column(in.src[0] + in.param);
        const std::span<atx::f64> dst_span = pool_.column(in.dst);
        const atx::usize n = dst_span.size();
        for (atx::usize ci = 0; ci < n; ++ci) {
          dst_span[ci] = src[ci];
        }
        continue;
      }
      (void)pool_.acquire();
      if (const atx::core::Status s = dispatch(in, dates, instruments, cells); !s) {
        return atx::core::Err(s.error());
      }
    }
    return atx::core::Ok(std::move(out));
  }

private:
  // The Program SlotId indexes the pool buffer directly (see evaluate()).
  [[nodiscard]] std::span<atx::f64> dst_col(const Instr &in) { return pool_.column(in.dst); }
  [[nodiscard]] std::span<const atx::f64> src_col(const Instr &in, atx::usize k) const {
    return pool_.column(in.src.at(k));
  }

  // Resolve each program field name -> the Panel's FieldId ONCE, into the
  // pre-sized scratch `field_remap_` (indexed by the program's field id). The
  // only allocation here is the scratch resize, which grows monotonically.
  [[nodiscard]] atx::core::Status resolve_fields(const Program &prog) {
    field_remap_.assign(prog.fields.size(), FieldId{0});
    for (atx::usize i = 0; i < prog.fields.size(); ++i) {
      ATX_TRY(const FieldId pid, panel_.field_id(prog.fields[i]));
      field_remap_[i] = pid;
    }
    return atx::core::Ok();
  }

  // (Re)create the SlotPool only when a program needs more slots than any prior
  // call (or the cell count changed). A warm same-shape call reuses the buffer.
  void ensure_pool(atx::u32 num_slots, atx::usize cells) {
    const atx::usize want_slots = num_slots == 0 ? atx::usize{1} : num_slots;
    const atx::usize want_cells = cells == 0 ? atx::usize{1} : cells;
    if (want_slots > pool_.capacity() || want_cells != pool_.cells_per_slot()) {
      pool_ = SlotPool{want_slots, want_cells};
    }
  }

  // ---- StoreAlpha — copy a slot's buffer into its output alpha column -------
  [[nodiscard]] atx::core::Status store_alpha(const Instr &in, SignalSet &out, atx::usize cells) {
    if (in.param >= out.alphas.size()) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "Engine::evaluate: StoreAlpha output index out of range");
    }
    const std::span<const atx::f64> src = src_col(in, 0);
    std::vector<atx::f64> &dst = out.alphas[in.param].values;
    for (atx::usize i = 0; i < cells; ++i) {
      dst[i] = src[i];
    }
    return atx::core::Ok();
  }

  // =======================================================================
  //  dispatch — the canonical EXHAUSTIVE switch over OpCode (NO default).
  //  StoreAlpha/Free are handled by evaluate(); they appear here only to keep
  //  the switch total and are unreachable. Cs*/Ts* return NotImplemented.
  // =======================================================================
  [[nodiscard]] atx::core::Status dispatch(const Instr &in, atx::usize dates,
                                           atx::usize instruments, atx::usize cells) {
    switch (in.op) {
    case OpCode::LoadField:
      return eval_load_field(in, dates, instruments);
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
      return eval_select(in, cells);
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
      return eval_cross_section(in, dates, instruments);
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
    // OU rolling-fit ops (P3d-E3): same windowed path as Ts* rolling ops.
    case OpCode::OuTheta:
    case OpCode::OuHalflife:
    case OpCode::OuMean:
    case OpCode::OuZscore:
      return eval_time_series(in, dates, instruments);
    case OpCode::TradeWhen:
    case OpCode::Hump:
    case OpCode::KalmanLevel:
    case OpCode::OuFilter:
      return eval_recurrence(in, dates, instruments);
    case OpCode::Split2:
      return eval_split2(in, cells);
    case OpCode::KalmanReg:
      return eval_kalman_reg(in, dates, instruments);
    case OpCode::Pin:
    case OpCode::StoreAlpha:
    case OpCode::Free:
      ATX_UNREACHABLE(); // Pin/StoreAlpha/Free handled by evaluate(); never dispatched
    }
    ATX_UNREACHABLE(); // exhaustive switch — no valid fallthrough
  }

  // ---- leaves -------------------------------------------------------------
  [[nodiscard]] atx::core::Status eval_const(const Instr &in) {
    const std::span<atx::f64> out = dst_col(in);
    for (atx::f64 &c : out) {
      c = in.imm[0];
    }
    return atx::core::Ok();
  }

  // LoadField copies the field, NaN-ing any out-of-universe cell (point-in-time).
  // `in.param` indexes the program's field dictionary; remap to the Panel id.
  [[nodiscard]] atx::core::Status eval_load_field(const Instr &in, atx::usize dates,
                                                  atx::usize instruments) {
    if (in.param >= field_remap_.size()) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "Engine::evaluate: LoadField param out of field-dictionary range");
    }
    const std::span<atx::f64> out = dst_col(in);
    const std::span<const atx::f64> field = panel_.field_all(field_remap_[in.param]);
    for (atx::usize d = 0; d < dates; ++d) {
      for (atx::usize j = 0; j < instruments; ++j) {
        const atx::usize idx = d * instruments + j;
        out[idx] = panel_.in_universe(d, j) ? field[idx] : detail::kVmNaN;
      }
    }
    return atx::core::Ok();
  }

  // ---- element-wise unary -------------------------------------------------
  [[nodiscard]] atx::core::Status eval_unary(const Instr &in) {
    const std::span<const atx::f64> a = src_col(in, 0);
    const std::span<atx::f64> out = dst_col(in);
    switch (in.op) {
    case OpCode::Neg:
      detail::vm_map_unary(a, out, [](atx::f64 x) noexcept { return -x; });
      break;
    case OpCode::Abs:
      detail::vm_map_unary(a, out, [](atx::f64 x) noexcept { return std::fabs(x); });
      break;
    case OpCode::Sign:
      detail::vm_map_unary(a, out, detail::vm_sign);
      break;
    case OpCode::Log:
      detail::vm_map_unary(a, out, [](atx::f64 x) noexcept { return std::log(x); });
      break;
    case OpCode::Sigmoid:
      // 1/(1+exp(-x)); NaN -> NaN naturally. Bit-identical to oracle's op_sigmoid.
      detail::vm_map_unary(a, out, [](atx::f64 x) noexcept { return 1.0 / (1.0 + std::exp(-x)); });
      break;
    case OpCode::Tanh:
      detail::vm_map_unary(a, out, [](atx::f64 x) noexcept { return std::tanh(x); });
      break;
    default:
      ATX_UNREACHABLE();
    }
    return atx::core::Ok();
  }

  // ---- element-wise binary ------------------------------------------------
  [[nodiscard]] atx::core::Status eval_binary(const Instr &in) {
    const std::span<const atx::f64> a = src_col(in, 0);
    const std::span<const atx::f64> b = src_col(in, 1);
    const std::span<atx::f64> out = dst_col(in);
    switch (in.op) {
    case OpCode::Add:
      detail::vm_map_binary(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x + y; });
      break;
    case OpCode::Sub:
      detail::vm_map_binary(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x - y; });
      break;
    case OpCode::Mul:
      detail::vm_map_binary(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x * y; });
      break;
    case OpCode::Div:
      detail::vm_map_binary(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x / y; });
      break;
    case OpCode::Pow:
      detail::vm_map_binary(a, b, out,
                            [](atx::f64 x, atx::f64 y) noexcept { return std::pow(x, y); });
      break;
    case OpCode::Spow:
      detail::vm_map_binary(a, b, out, detail::vm_spow);
      break;
    case OpCode::MinP:
      detail::vm_map_binary(a, b, out, detail::vm_min);
      break;
    case OpCode::MaxP:
      detail::vm_map_binary(a, b, out, detail::vm_max);
      break;
    default:
      ATX_UNREACHABLE();
    }
    return atx::core::Ok();
  }

  // ---- comparisons --------------------------------------------------------
  [[nodiscard]] atx::core::Status eval_cmp(const Instr &in) {
    const std::span<const atx::f64> a = src_col(in, 0);
    const std::span<const atx::f64> b = src_col(in, 1);
    const std::span<atx::f64> out = dst_col(in);
    switch (in.op) {
    case OpCode::CmpLt:
      detail::vm_map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x < y; });
      break;
    case OpCode::CmpGt:
      detail::vm_map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x > y; });
      break;
    case OpCode::CmpLe:
      detail::vm_map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x <= y; });
      break;
    case OpCode::CmpGe:
      detail::vm_map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x >= y; });
      break;
    case OpCode::CmpEq:
      detail::vm_map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x == y; });
      break;
    case OpCode::CmpNe:
      detail::vm_map_cmp(a, b, out, [](atx::f64 x, atx::f64 y) noexcept { return x != y; });
      break;
    default:
      ATX_UNREACHABLE();
    }
    return atx::core::Ok();
  }

  // ---- logical / not / select ---------------------------------------------
  [[nodiscard]] atx::core::Status eval_logical(const Instr &in) {
    detail::vm_map_binary(src_col(in, 0), src_col(in, 1), dst_col(in),
                          in.op == OpCode::And ? detail::vm_and : detail::vm_or);
    return atx::core::Ok();
  }

  [[nodiscard]] atx::core::Status eval_not(const Instr &in) {
    detail::vm_map_unary(src_col(in, 0), dst_col(in), detail::vm_not);
    return atx::core::Ok();
  }

  [[nodiscard]] atx::core::Status eval_select(const Instr &in, atx::usize cells) {
    const std::span<const atx::f64> c = src_col(in, 0);
    const std::span<const atx::f64> a = src_col(in, 1);
    const std::span<const atx::f64> b = src_col(in, 2);
    const std::span<atx::f64> out = dst_col(in);
    for (atx::usize i = 0; i < cells; ++i) {
      out[i] = detail::vm_select(c[i], a[i], b[i]);
    }
    return atx::core::Ok();
  }

  // ---- cross-sectional (per date-row) -------------------------------------
  // Slice each whole-panel slot buffer to a single date row and apply the
  // cross-sectional kernel (cs_ops.hpp) over that row's VALID SET (non-NaN
  // cells). `x` is the input (src[0]); group ops take the classifier in
  // src[1], CsScale takes the scalar factor a == src[1][0]. EVERY output cell
  // is written (out-of-set -> NaN) since scratch slots are recycled. Mirrors
  // oracle.hpp's Oracle::eval_cross_section / cs_one_date dispatch exactly.
  [[nodiscard]] atx::core::Status eval_cross_section(const Instr &in, atx::usize dates,
                                                     atx::usize instruments) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const std::span<atx::f64> out = dst_col(in);
    const bool grouped =
        (in.op == OpCode::CsDemeanG || in.op == OpCode::CsNeutG || in.op == OpCode::CsRankG ||
         in.op == OpCode::CsZscoreG || in.op == OpCode::CsCountG || in.op == OpCode::CsMeanG ||
         in.op == OpCode::CsScaleG);
    std::span<const atx::f64> g{};
    // The scalar 2nd operand: CsScale's target L1 norm `a`, or CsWinsorize's
    // std multiplier `k`. Read EXACTLY as CsScale does (cell [0] of the slot).
    atx::f64 scale_a = 1.0;
    if (grouped) {
      g = src_col(in, 1);
    } else if (in.op == OpCode::CsScale || in.op == OpCode::CsWinsorize) {
      const std::span<const atx::f64> col = src_col(in, 1);
      scale_a = col.empty() ? detail::kVmNaN : col.front();
    }
    std::vector<atx::usize> valid; // per-row scratch, reused across dates
    valid.reserve(instruments);
    for (atx::usize d = 0; d < dates; ++d) {
      const std::span<const atx::f64> xr = x.subspan(d * instruments, instruments);
      const std::span<atx::f64> orow = out.subspan(d * instruments, instruments);
      const std::span<const atx::f64> grow =
          grouped ? g.subspan(d * instruments, instruments) : std::span<const atx::f64>{};
      cs_one_date(in.op, xr, grow, scale_a, orow, valid);
    }
    return atx::core::Ok();
  }

  // Apply one cross-sectional op to a single date's row. `out` is reset to all
  // NaN here (out-of-set cells stay NaN — scratch slots are recycled), the
  // valid set is rebuilt into `valid` (caller-owned scratch), then dispatched.
  static void cs_one_date(OpCode op, std::span<const atx::f64> x, std::span<const atx::f64> g,
                          atx::f64 scale_a, std::span<atx::f64> out,
                          std::vector<atx::usize> &valid) {
    valid.clear();
    for (atx::usize i = 0; i < x.size(); ++i) {
      out[i] = detail::kVmNaN; // default every cell (out-of-set stays NaN)
      if (!detail::cs_is_nan(x[i])) {
        valid.push_back(i);
      }
    }
    switch (op) {
    case OpCode::CsRank:
      detail::cs_rank_row(x, valid, out);
      break;
    case OpCode::CsZscore:
      detail::cs_zscore_row(x, valid, out);
      break;
    case OpCode::CsScale:
      detail::cs_scale_row(x, valid, scale_a, out);
      break;
    case OpCode::CsNormalize:
      detail::cs_normalize_row(x, valid, out);
      break;
    case OpCode::CsWinsorize:
      detail::cs_winsorize_row(x, valid, scale_a, out);
      break;
    case OpCode::CsDemeanG:
    case OpCode::CsNeutG: // SAFETY: residualize-on-group-dummies == per-group demean
      detail::cs_group_demean_row(x, g, valid, out);
      break;
    case OpCode::CsRankG:
      detail::cs_group_row(x, g, valid, out, /*zscore=*/false);
      break;
    case OpCode::CsZscoreG:
      detail::cs_group_row(x, g, valid, out, /*zscore=*/true);
      break;
    case OpCode::CsCountG:
      detail::cs_group_count_mean_row(x, g, valid, out, /*want_mean=*/false);
      break;
    case OpCode::CsMeanG:
      detail::cs_group_count_mean_row(x, g, valid, out, /*want_mean=*/true);
      break;
    case OpCode::CsScaleG:
      detail::cs_group_scale_row(x, g, valid, out);
      break;
    default:
      ATX_UNREACHABLE();
    }
  }

  // ---- time-series (per instrument column, causal trailing window) ---------
  // Resolve the window `d` from the op's LAST operand, then fill every output
  // cell from ts_ops.hpp's per-cell kernels. The window is STRIDED by
  // `instruments` down each instrument column; the kernels recompute over the
  // trailing window [t-d+1, t] in the oracle's chronological order (NO online
  // rolling — bit-exact with oracle.hpp). Iterate instrument-outer / date-inner
  // to match the oracle's loop nest. Mirrors oracle.hpp's eval_time_series.
  [[nodiscard]] atx::core::Status eval_time_series(const Instr &in, atx::usize dates,
                                                   atx::usize instruments) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const std::span<atx::f64> out = dst_col(in);
    // Window from the LAST populated operand (delay/delta/unary-window: src[1];
    // corr/cov: src[2]). Find the highest non-kNoSlot operand slot.
    atx::usize last = 0;
    for (atx::usize k = 0; k < in.src.size(); ++k) {
      if (in.src.at(k) != kNoSlot) {
        last = k;
      }
    }
    const atx::usize d = detail::tsv_window_of(src_col(in, last));
    const bool binary_series = (in.op == OpCode::TsCorr || in.op == OpCode::TsCov);
    const std::span<const atx::f64> y =
        binary_series ? src_col(in, 1) : std::span<const atx::f64>{};
    // OU rolling-fit ops (P3d-E4) fit AR(1) over the trailing window per cell;
    // they take the same windowed path but a distinct per-cell kernel.
    const bool ou_rolling = (in.op == OpCode::OuTheta || in.op == OpCode::OuHalflife ||
                             in.op == OpCode::OuMean || in.op == OpCode::OuZscore);

    // Reusable scratch sized to the window: NO per-cell allocation (grown only
    // when `d` exceeds any prior call). Only the sort/pair ops touch it.
    if (d > ts_scratch_a_.size()) {
      ts_scratch_a_.resize(d);
      ts_scratch_b_.resize(d);
    }
    for (atx::usize j = 0; j < instruments; ++j) {
      for (atx::usize t = 0; t < dates; ++t) {
        out[t * instruments + j] =
            ou_rolling
                ? detail::ou_value_at(in.op, x, t, j, d, instruments, ts_scratch_a_)
            : binary_series ? detail::ts_pair_at(in.op, x, y, t, j, d, instruments, ts_scratch_a_,
                                                 ts_scratch_b_)
                            : detail::ts_value_at(in.op, x, t, j, d, instruments, ts_scratch_a_);
      }
    }
    return atx::core::Ok();
  }

  // ---- stateful recurrence (forward scan, true cross-date state) -----------
  // trade_when / hump carry state from the panel's FIRST date forward (no
  // trailing window), so they CANNOT use eval_time_series. Per instrument j, we
  // walk dates t=0…D-1 in order, holding the prior output out[t-1,j] in a pooled
  // `state_[j]` slot (sized once, reused across calls — ZERO hot-path alloc),
  // compute out[t,j] from state_[j] + the date-t operand cells, then advance
  // state_[j] = out[t,j]. trade_when reads trigger=src[0], alpha=src[1],
  // exit=src[2]; hump reads x=src[0] and the scalar threshold from src[1][0]
  // (read EXACTLY as winsorize/CsScale read their scalar 2nd operand), defaulting
  // to 0.01 when the optional arg is absent (P3b-1 default-fill normally
  // materializes it, so the absent path is defensive).
  //
  // SAFETY: causal BY CONSTRUCTION. The scan reads state_[j] (which holds
  // out[t-1,j]) and inputs at the flat index t*I+j (date t). There is NO index
  // into state_[>t] or any input at a date > t — a forward reference is
  // unrepresentable. The first date (t==0) is special-cased, so state_ needs no
  // separate clear: t==0 seeds it. state_[j] reuse across t (and across calls,
  // after the grow-once resize) is sound because every read of state_[j] at date
  // t precedes its write for date t.
  [[nodiscard]] atx::core::Status eval_recurrence(const Instr &in, atx::usize dates,
                                                  atx::usize instruments) {
    const std::span<atx::f64> out = dst_col(in);
    if (in.op == OpCode::KalmanLevel) {
      return eval_kalman_level(in, out, dates, instruments);
    }
    if (in.op == OpCode::OuFilter) {
      return eval_ou_filter(in, out, dates, instruments);
    }
    if (instruments > state_.size()) {
      state_.resize(instruments); // grow-once; reused across calls
    }
    if (in.op == OpCode::Hump) {
      const std::span<const atx::f64> x = src_col(in, 0);
      const atx::f64 thr = in.src.at(1) == kNoSlot ? atx::f64{0.01} : src_col(in, 1).front();
      for (atx::usize j = 0; j < instruments; ++j) {
        for (atx::usize t = 0; t < dates; ++t) {
          const atx::usize i = t * instruments + j;
          const atx::f64 v = detail::hump_step(state_[j], x[i], thr, /*first=*/t == 0);
          out[i] = v;
          state_[j] = v;
        }
      }
      return atx::core::Ok();
    }
    // TradeWhen: trigger=src[0], alpha=src[1], exit=src[2].
    const std::span<const atx::f64> trig = src_col(in, 0);
    const std::span<const atx::f64> alpha = src_col(in, 1);
    const std::span<const atx::f64> exit_v = src_col(in, 2);
    for (atx::usize j = 0; j < instruments; ++j) {
      for (atx::usize t = 0; t < dates; ++t) {
        const atx::usize i = t * instruments + j;
        const atx::f64 v =
            detail::trade_when_step(state_[j], trig[i], exit_v[i], alpha[i], /*first=*/t == 0);
        out[i] = v;
        state_[j] = v;
      }
    }
    return atx::core::Ok();
  }

  // KalmanLevel VM kernel — per-instrument forward scan using the shared scalar
  // step kernel (state_ops::kalman_level_step). Stack-local KalmanLevelState per
  // instrument (no pooled state_ buffer needed — the struct holds {x,P}). Reads
  // Q/R from in.imm[0/1]. The oracle restates this math INLINE for the diff.
  [[nodiscard]] atx::core::Status eval_kalman_level(const Instr &in, std::span<atx::f64> out,
                                                    atx::usize dates, atx::usize instruments) {
    const std::span<const atx::f64> z = src_col(in, 0);
    const atx::f64 Q = in.imm[0];
    const atx::f64 R = in.imm[1];
    for (atx::usize j = 0; j < instruments; ++j) {
      detail::KalmanLevelState s{};
      bool seeded = false;
      for (atx::usize t = 0; t < dates; ++t) {
        const atx::usize i = t * instruments + j;
        out[i] = detail::kalman_level_step(s, seeded, z[i], Q, R);
      }
    }
    return atx::core::Ok();
  }

  // OuFilter VM kernel — per-instrument forward scan using the shared scalar step
  // kernel (state_ops::ou_filter_step). Stack-local {xhat, seeded} per instrument.
  // Reads theta/mu from in.imm[0/1]. The oracle restates this math INLINE.
  [[nodiscard]] atx::core::Status eval_ou_filter(const Instr &in, std::span<atx::f64> out,
                                                 atx::usize dates, atx::usize instruments) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const atx::f64 theta = in.imm[0];
    const atx::f64 mu = in.imm[1];
    for (atx::usize j = 0; j < instruments; ++j) {
      atx::f64 xhat = 0.0;
      bool seeded = false;
      for (atx::usize t = 0; t < dates; ++t) {
        const atx::usize i = t * instruments + j;
        out[i] = detail::ou_filter_step(xhat, seeded, x[i], theta, mu);
      }
    }
    return atx::core::Ok();
  }

  // ---- multi-output (Split2) -----------------------------------------------
  // Split2 is the synthetic test op: hi = x, lo = -x. It occupies a contiguous
  // two-slot block [dst, dst+1]; `out_col(in, k)` = pool_.column(in.dst + k).
  // SAFETY: the linearizer's acquire_block(2) ensured both buffer slots are
  // within the pre-sized pool; accessing in.dst+1 never exceeds capacity.
  [[nodiscard]] atx::core::Status eval_split2(const Instr &in, atx::usize cells) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const std::span<atx::f64> hi = pool_.column(in.dst + 0);
    const std::span<atx::f64> lo = pool_.column(in.dst + 1);
    for (atx::usize i = 0; i < cells; ++i) {
      hi[i] = x[i];
      lo[i] = -x[i];
    }
    return atx::core::Ok();
  }

  // ---- Chan 2-state time-varying regression (multi-output, 3 pins) -----------
  // Writes alpha(dst+0), beta(dst+1), resid(dst+2) using the shared kernel from
  // state_ops.hpp (kalman_reg_step). Per instrument j: walk dates t=0…D-1 in
  // order with a fresh KalmanRegState (diffuse prior). delta=in.imm[0],
  // R=in.imm[1]. y=src[0], x=src[1]. Accesses pool_.column(dst+k) directly for
  // the three output columns — the linearizer's acquire_block(3) guarantees the
  // contiguous block [dst, dst+2] is within the pre-sized pool.
  // SAFETY: causal by construction (step reads only prior state + date-t inputs).
  [[nodiscard]] atx::core::Status eval_kalman_reg(const Instr &in, atx::usize dates,
                                                  atx::usize instruments) {
    const std::span<const atx::f64> y = src_col(in, 0);
    const std::span<const atx::f64> x = src_col(in, 1);
    const atx::f64 delta = in.imm[0];
    const atx::f64 R = in.imm[1];
    const std::span<atx::f64> oa = pool_.column(in.dst + 0);
    const std::span<atx::f64> ob = pool_.column(in.dst + 1);
    const std::span<atx::f64> orr = pool_.column(in.dst + 2);
    for (atx::usize j = 0; j < instruments; ++j) {
      detail::KalmanRegState s{};
      bool seeded = false;
      for (atx::usize t = 0; t < dates; ++t) {
        const atx::usize i = t * instruments + j;
        const detail::KalmanRegOut o = detail::kalman_reg_step(s, seeded, y[i], x[i], delta, R);
        oa[i] = o.alpha;
        ob[i] = o.beta;
        orr[i] = o.resid;
      }
    }
    return atx::core::Ok();
  }

  const Panel &panel_;
  SlotPool pool_{1, 1};                // reused across calls; grown on demand
  std::vector<FieldId> field_remap_;   // program field id -> Panel FieldId scratch
  std::vector<atx::f64> ts_scratch_a_; // Ts* window scratch (sort/corr/cov); grown on demand
  std::vector<atx::f64> ts_scratch_b_; // Ts* second-window scratch (corr/cov)
  std::vector<atx::f64> state_;        // recurrence state[n_instruments]; grown once, reused
};

} // namespace atx::engine::alpha
