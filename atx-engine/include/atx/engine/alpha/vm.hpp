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
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"

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
    for (const Instr &in : prog.code) {
      if (in.op == OpCode::Free) {
        pool_.release(in.dst);
        continue;
      }
      if (in.op == OpCode::StoreAlpha) {
        ATX_TRY_VOID(store_alpha(in, out, cells));
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
    case OpCode::CsDemeanG:
    case OpCode::CsNeutG:
    case OpCode::CsRankG:
    case OpCode::CsZscoreG:
      return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                            "Cs*: cross-sectional/time-series kernels land in P3-7/P3-8");
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
      return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                            "Ts*: cross-sectional/time-series kernels land in P3-7/P3-8");
    case OpCode::StoreAlpha:
    case OpCode::Free:
      ATX_UNREACHABLE(); // handled by evaluate(); never dispatched
    }
    ATX_UNREACHABLE(); // exhaustive switch — no valid fallthrough
  }

  // ---- leaves -------------------------------------------------------------
  [[nodiscard]] atx::core::Status eval_const(const Instr &in) {
    const std::span<atx::f64> out = dst_col(in);
    for (atx::f64 &c : out) {
      c = in.imm;
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

  const Panel &panel_;
  SlotPool pool_{1, 1};              // reused across calls; grown on demand
  std::vector<FieldId> field_remap_; // program field id -> Panel FieldId scratch
};

} // namespace atx::engine::alpha
