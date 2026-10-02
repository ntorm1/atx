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
// CACHE IDENTITY: the IC runner's candidate signal cache keys stored VM outputs
// by `dsl_vm_semantics_version` (atx-impl/src/strategy_ic_runner.cpp). Bump it
// with ANY change here, in the Cs/Ts kernels, or in parse/compile that can alter
// one evaluated bit; otherwise a reused cache serves pre-change signals.
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
//  Cross-sectional (Cs*) kernels are in cs_ops.hpp (P3-7); time-series (Ts*)
//  kernels are in ts_ops.hpp (P3-8). Both families are fully implemented; the
//  oracle differential (P3-9) enforces bit-exact agreement on the AuditExact path.
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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <xsimd/xsimd.hpp>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/cs_ops.hpp"
#include "atx/engine/alpha/fusion.hpp" // Lane 2: FusedProgram / FusedKernel
#include "atx/engine/alpha/lit_ops.hpp" // platform-v7 W2 literature kernels
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/state_ops.hpp"
#include "atx/engine/alpha/subtree_cache.hpp" // Lane 2: cross-program subtree cache
#include "atx/engine/alpha/ts_ops.hpp"

#include "atx/engine/parallel/det_pool.hpp" // optional intra-eval column pool (S3-3)

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

// THE element-wise kernel (Lane 2): one implementation shared by the unfused
// per-instruction path (eval_unary/binary/cmp/logical/not/select) and the fused
// micro-op path, so both apply the identical scalar function per cell. Operands
// follow the instruction's src order (Select: x = condition, y = then, z = else);
// unused operands are ignored. n cells from each pointer. Precondition: `op` is
// is_fusible_elementwise.
inline void vm_apply_ew(OpCode op, const atx::f64 *x, const atx::f64 *y, const atx::f64 *z,
                        atx::f64 *o, atx::usize n) noexcept {
  const std::span<const atx::f64> a{x, x == nullptr ? 0 : n};
  const std::span<const atx::f64> b{y, y == nullptr ? 0 : n};
  const std::span<atx::f64> out{o, n};
  switch (op) {
  case OpCode::Neg:
    vm_map_unary(a, out, [](atx::f64 v) noexcept { return -v; });
    return;
  case OpCode::Abs:
    vm_map_unary(a, out, [](atx::f64 v) noexcept { return std::fabs(v); });
    return;
  case OpCode::Sign:
    vm_map_unary(a, out, vm_sign);
    return;
  case OpCode::Log:
    vm_map_unary(a, out, [](atx::f64 v) noexcept { return std::log(v); });
    return;
  case OpCode::Sigmoid:
    // 1/(1+exp(-x)); NaN -> NaN naturally. Bit-identical to oracle's op_sigmoid.
    vm_map_unary(a, out, [](atx::f64 v) noexcept { return 1.0 / (1.0 + std::exp(-v)); });
    return;
  case OpCode::Tanh:
    vm_map_unary(a, out, [](atx::f64 v) noexcept { return std::tanh(v); });
    return;
  case OpCode::Add:
    vm_map_binary(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p + q; });
    return;
  case OpCode::Sub:
    vm_map_binary(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p - q; });
    return;
  case OpCode::Mul:
    vm_map_binary(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p * q; });
    return;
  case OpCode::Div:
    vm_map_binary(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p / q; });
    return;
  case OpCode::Pow:
    vm_map_binary(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return std::pow(p, q); });
    return;
  case OpCode::Spow:
    vm_map_binary(a, b, out, vm_spow);
    return;
  case OpCode::MinP:
    vm_map_binary(a, b, out, vm_min);
    return;
  case OpCode::MaxP:
    vm_map_binary(a, b, out, vm_max);
    return;
  case OpCode::CmpLt:
    vm_map_cmp(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p < q; });
    return;
  case OpCode::CmpGt:
    vm_map_cmp(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p > q; });
    return;
  case OpCode::CmpLe:
    vm_map_cmp(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p <= q; });
    return;
  case OpCode::CmpGe:
    vm_map_cmp(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p >= q; });
    return;
  case OpCode::CmpEq:
    vm_map_cmp(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p == q; });
    return;
  case OpCode::CmpNe:
    vm_map_cmp(a, b, out, [](atx::f64 p, atx::f64 q) noexcept { return p != q; });
    return;
  case OpCode::And:
    vm_map_binary(a, b, out, vm_and);
    return;
  case OpCode::Or:
    vm_map_binary(a, b, out, vm_or);
    return;
  case OpCode::Not:
    vm_map_unary(a, out, vm_not);
    return;
  case OpCode::Select:
    for (atx::usize i = 0; i < n; ++i) {
      o[i] = vm_select(x[i], y[i], z[i]);
    }
    return;
  default:
    ATX_UNREACHABLE(); // precondition: an element-wise opcode
  }
}

} // namespace detail

// =========================================================================
//  EvalMode — the two-tier determinism contract (p7 ROADMAP §Shared
//  determinism contract; p6 S1-0 plumbing pattern).
//
//  AuditExact (DEFAULT, inert): the engine's pinned contract verbatim — every Ts*
//  kernel takes the batch (oracle-bit-exact) path, output byte-identical across
//  all worker counts and identical to pre-sprint. This is the publication path.
//
//  ResearchFast (opt-in): perf wins that change bits ship here — e.g. the p7 S3-1
//  Welford/Neumaier online variance family (ts_ops.hpp tsv_welford_*), whose
//  accumulation order differs from the oracle's two-pass chronological recompute.
//  Provably more accurate, NOT bit-identical, so gated. Alphas discovered under
//  ResearchFast are re-scored on the AuditExact path before publication.
//
//  The mode is a per-Engine field defaulting to AuditExact, so the no-flag path is
//  byte-identical to pre-sprint. Set via Engine::set_eval_mode before evaluate().
// =========================================================================
enum class EvalMode : atx::u8 {
  AuditExact = 0, // default, inert: batch kernels, oracle-bit-exact
  ResearchFast,   // opt-in: online variance family (tolerance, not bit-exact)
};

// =========================================================================
//  KernelPolicy (W0-A0) — the versioned numeric policies of the kernel fixes.
//
//  Every default is the corrected behaviour: average-rank ties (A-01), the
//  seeding / NaN-emitting / stale-capped hump (A-02), the flat-window guard
//  (A-09) and oracle-exact windowed AuditExact ts_sum/ts_mean with a Neumaier
//  ResearchFast slide (A-13). legacy_v1() selects every pre-W0 rule at once and
//  re-derives pre-W0 digests bit-exactly (AlphaCsRankTies_Digest and siblings).
//  The oracle pins the default policy only. A non-default policy bypasses the
//  SubtreeCache (its key does not carry the policy).
// =========================================================================
using RankTies = detail::RankTies;
using HumpNaN = detail::HumpNaN;
using FlatGuard = detail::FlatGuard;
using TsSumPath = detail::TsSumPath;

struct KernelPolicy {
  RankTies rank_ties{RankTies::Average};
  HumpNaN hump{HumpNaN::SeedCapV2};
  FlatGuard flat{FlatGuard::RelativeV2};
  TsSumPath ts_sum{TsSumPath::WindowedV2};

  // Every pre-W0 rule (the policy the old golden digests were produced under).
  [[nodiscard]] static constexpr KernelPolicy legacy_v1() noexcept {
    return KernelPolicy{RankTies::OrdinalV1, HumpNaN::StickyV1, FlatGuard::NoneV1,
                        TsSumPath::OnlineV1};
  }
  [[nodiscard]] constexpr bool is_default() const noexcept {
    return rank_ties == RankTies::Average && hump == HumpNaN::SeedCapV2 &&
           flat == FlatGuard::RelativeV2 && ts_sum == TsSumPath::WindowedV2;
  }
};

// =========================================================================
//  Range execution vocabulary (Lane 2 — strategy B / level-scheduled eval).
//
//  Every opcode's kernel is independent along ONE axis of the date-major panel:
//    * Cells       — element-wise maps, Const, Select, Pin, Split2: cell i depends
//                    only on input cell i;
//    * Dates       — LoadField and every Cs* op: a date row depends only on that
//                    date's rows;
//    * Instruments — every Ts*/OU/recurrence/Kalman op: instrument column j
//                    depends only on input column j.
//  A kernel executed over a sub-range [lo, hi) of its axis therefore writes
//  exactly the cells the full-range call writes for that range, with the SAME
//  per-cell arithmetic in the SAME order — so any partition of the axis across
//  workers is bit-identical to the serial full-range call.
// =========================================================================
enum class ChunkAxis : atx::u8 { Cells, Dates, Instruments };

// A borrowed slot buffer for Engine::execute_range: `rd` is the readable view,
// `wr` the writable one (null for read-only inputs such as a cache entry). `n`
// is the buffer length in cells.
struct ExtSlot {
  const atx::f64 *rd{nullptr};
  atx::f64 *wr{nullptr};
  atx::usize n{0};
};

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

  // The capacity (max simultaneously-live slots) of the SlotPool's backing
  // storage. Stable as long as no reallocation occurs (i.e. same num_slots and
  // cells between calls). Exposed so tests can probe for reallocation without
  // touching raw internals: stable capacity ⟺ no realloc (ensure_pool only
  // grows when want_slots > capacity or cells_per_slot changes).
  [[nodiscard]] atx::usize pool_capacity() const noexcept { return pool_.capacity(); }

  // EvalMode plumbing (p7 S3-1). The mode selects the Ts* variance-family
  // execution tier: AuditExact (default) keeps the batch kernel byte-identical to
  // the oracle; ResearchFast routes TsVar/TsStd/TsZscore/TsAvDiff through the O(T)
  // Welford/Neumaier online sweep (faster, tolerance-conformant, NOT bit-exact).
  // Set BEFORE evaluate(); affects only the variance-family ops, nothing else.
  void set_eval_mode(EvalMode mode) noexcept { mode_ = mode; }
  [[nodiscard]] EvalMode eval_mode() const noexcept { return mode_; }

  // W0-A0 kernel policy (see KernelPolicy). Default-constructed == corrected
  // behaviour; KernelPolicy::legacy_v1() re-derives pre-W0 outputs. Set BEFORE
  // evaluate().
  void set_kernel_policy(KernelPolicy policy) noexcept { policy_ = policy; }
  [[nodiscard]] KernelPolicy kernel_policy() const noexcept { return policy_; }

  // Optional date-major eligibility for cross-sectional operations ONLY. Field
  // loads retain the Panel's observation mask, so a newly eligible name can use
  // its observed price history in a trailing time-series window. Every Cs* op
  // excludes ineligible names from its reductions and emits NaN for them.
  // Call before evaluation (never concurrently). Takes ownership; empty restores
  // the default valid-set behavior. A nonempty mask must contain cells() values
  // in {0,1}; invalid input returns Err without changing the current mask.
  // Subtree caching is bypassed while masked: its key has no eligibility field.
  [[nodiscard]] atx::core::Status set_cross_section_mask(std::vector<atx::u8> mask) {
    if ((!mask.empty() && mask.size() != panel_.cells()) ||
        std::any_of(mask.begin(), mask.end(), [](atx::u8 v) { return v > 1U; })) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Engine: cross-section mask must be empty or cells() binary flags");
    }
    cs_mask_ = std::move(mask);
    return atx::core::Ok();
  }

  // Cross-instrument column parallelism (p7 S3-3). When a non-null DetPool is set,
  // the BATCH Ts path (eval_time_series's column-extract loop) dispatches its
  // instrument columns across the pool's workers; each column is independent
  // (out[t*I+j] depends only on input column j, disjoint output slots, per-worker
  // scratch), so the result is bit-identical to the serial loop — AuditExact. When
  // null (the DEFAULT), eval_time_series runs the original single-threaded loop,
  // byte-for-byte unchanged. Set BEFORE evaluate(); the Engine BORROWS the pool
  // (non-owning) for the duration — the caller owns its lifetime.
  //
  // DEADLOCK CONSTRAINT (REQUIRED): this pool MUST be a SEPARATE instance from any
  // outer search-level DetPool driving genome-level parallelism. A DetPool worker
  // that calls back into the SAME pool's parallel_for would block on a barrier that
  // can never complete (all workers parked inside the outer job). Wiring this into
  // the search driver (which already nests a det_pool) is deferred to S7 and must
  // construct a distinct column pool per worker thread.
  void set_ts_pool(atx::engine::parallel::DetPool *pool) {
    ts_pool_ = pool;
    // Pre-size the OUTER per-worker scratch vectors to n_workers up front so no
    // thread ever resizes the shared outer vector (only its OWN inner vector grows,
    // inside the band body). A null pool needs no per-worker scratch.
    const atx::usize w = (pool != nullptr) ? pool->n_workers() : atx::usize{0};
    if (ts_col_thr_.size() < w) {
      ts_col_thr_.resize(w);
      ts_col_b_thr_.resize(w);
      ts_scratch_a_thr_.resize(w);
      ts_scratch_b_thr_.resize(w);
    }
  }
  [[nodiscard]] atx::engine::parallel::DetPool *ts_pool() const noexcept { return ts_pool_; }

  // Prepare the Engine to evaluate a DIFFERENT Program on the SAME Panel.
  //
  // WHY THIS EXISTS: `evaluate()` already reuses the SlotPool and per-date Cs*
  // scratch across successive calls on the same Program. But each `evaluate()`
  // call re-runs `resolve_fields()`, which re-maps the new Program's field names
  // to Panel FieldIds and rebuilds `field_remap_`. For a factory worker that
  // evaluates many distinct Programs on one Panel, paying the field-remap cost
  // per call is fine — `reset()` exists so S2 can hoist one warm Engine per
  // worker and skip re-constructing it between genomes.
  //
  // WHAT IS CLEARED:
  //   * `field_remap_` — cleared (zeroed) so `resolve_fields()` rebuilds it for
  //     the next program's field set. A stale remap from a prior program that
  //     silently shadows the new program's fields is a Critical bug; clearing it
  //     makes any accidental reuse of stale data crash loudly on a bounds check.
  //
  // WHAT IS RETAINED (no reallocation):
  //   * `pool_` — the SlotPool's backing storage and capacity are kept. Its
  //     live-slot counter is already 0 after a normal successful evaluate()
  //     (every acquire has a matching Free/release — see the dispatch loop).
  //     reset() does NOT call reset_live(); if the next program needs MORE slots
  //     or a different cell count, `ensure_pool()` will grow it.
  //   * All other scratch buffers (`ts_scratch_a_/b_`, `ts_dq_lo_/hi_`, `state_`,
  //     `cs_valid_`, `cs_scratch_`, `ts_col_`, `ts_col_b_`) — retained at their
  //     current capacity. They grow monotonically and are cleared/rebuilt at the
  //     start of each operation that uses them, so stale content never leaks into
  //     a computation.
  //
  // PRECONDITIONS:
  //   * The Engine is bound to the SAME Panel (structurally enforced — there is
  //     no way to rebind the panel).
  //   * Call only after a successful evaluate() or on a fresh Engine, where
  //     pool_.live()==0 already holds: every slot acquired during evaluate() is
  //     released by the corresponding Free instruction in the same dispatch loop,
  //     so a complete evaluate() always exits with live()==0. reset() therefore
  //     only needs to clear the per-program field remap.
  //
  // EXCEPTION SAFETY: noexcept — only zeroes a vector's contents (capacity
  // kept), which does not throw.
  void reset() noexcept {
    // PRECONDITION CHECK: a clean reset assumes the prior evaluate() balanced every
    // acquire() with a Free/release (live()==0). This holds after a successful
    // evaluate() by the linearizer's one-Free-per-live-node contract; the assert
    // fails loud if a caller reset()s after a partial/errored evaluate. Uses the
    // pre-existing SlotPool::live() accessor — no pool mutation, no panel.hpp edit.
    ATX_ASSERT(pool_.live() == 0);
    // Clear the field remap so the next resolve_fields() cannot accidentally read a
    // stale FieldId from a prior program. The vector retains its storage capacity.
    // This is reset()'s ONLY substantive action: the pool and every other scratch
    // buffer are already in a clean-start state (see contract above). All other
    // scratch buffers (ts_scratch_*, ts_dq_*, state_, cs_valid_, cs_scratch_,
    // ts_col_*, ts_col_b_) grow monotonically and are fully overwritten before
    // any read on the next evaluate().
    field_remap_.clear();
  }

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
        eval_pin(in, 0, cells);
        continue;
      }
      (void)pool_.acquire();
      if (const atx::core::Status s = dispatch(in, dates, instruments, cells); !s) {
        return atx::core::Err(s.error());
      }
    }
    return atx::core::Ok(std::move(out));
  }

  // =======================================================================
  //  Lane 2 — cache-aware / subset evaluation.
  //
  //  evaluate(prog, cache): every root, reusing and publishing subtree results in
  //    `cache` (null cache == the plain evaluate(prog) above, byte-identical).
  //  evaluate_nodes(prog, roots, cache): ONLY the listed roots (indices into
  //    prog.roots, unique, any order) — instructions no requested root depends on
  //    are skipped (dead-code eliminated at run time), and a subtree found in the
  //    cache is copied in instead of recomputed, which also skips its whole
  //    producer cone. Output alphas follow `roots` order.
  //  evaluate_root(prog, root, cache): one root into an Engine-owned buffer; the
  //    span stays valid until the next evaluate_* call on this Engine.
  //
  //  Bit-identity: a skipped instruction's value is never read (liveness is exact);
  //  a cache hit is the byte-exact buffer a fresh evaluation of the same subtree on
  //  the same panel + mode produced. So the output equals evaluate(prog)'s alphas
  //  byte-for-byte (tests: SubtreeCache_*, AlphaVmNodes_*).
  //
  //  Errors: Err(InvalidArgument) for an out-of-range / duplicate root index, plus
  //  every evaluate() error. Allocation: planning scratch grows monotonically; each
  //  publish copies the value (cold relative to the kernel that produced it).
  // =======================================================================
  [[nodiscard]] atx::core::Result<SignalSet> evaluate(const Program &prog, SubtreeCache *cache) {
    if (cache == nullptr) {
      return evaluate(prog);
    }
    all_roots_.resize(prog.roots.size());
    for (atx::usize r = 0; r < prog.roots.size(); ++r) {
      all_roots_[r] = static_cast<atx::u32>(r);
    }
    return evaluate_nodes(prog, all_roots_, cache);
  }

  [[nodiscard]] atx::core::Result<SignalSet>
  evaluate_nodes(const Program &prog, std::span<const atx::u32> roots, SubtreeCache *cache) {
    if (!policy_.is_default() || !cs_mask_.empty()) {
      cache = nullptr; // SubtreeKey carries neither KernelPolicy nor CS eligibility.
    }
    const atx::usize dates = panel_.dates();
    const atx::usize instruments = panel_.instruments();
    const atx::usize cells = dates * instruments;
    ATX_TRY_VOID(resolve_fields(prog));
    ensure_pool(prog.num_slots, cells);
    ATX_TRY_VOID(map_requested_roots(prog, roots));
    plan_needed(prog, cache);

    SignalSet out;
    out.dates = dates;
    out.instruments = instruments;
    out.alphas.resize(roots.size());
    for (atx::usize k = 0; k < roots.size(); ++k) {
      out.alphas[k].name = prog.roots[roots[k]].name;
      out.alphas[k].values.assign(cells, detail::kVmNaN);
    }
    for (atx::usize i = 0; i < prog.code.size(); ++i) {
      const Instr &in = prog.code[i];
      if (in.op == OpCode::Free) {
        pool_.release(in.dst);
        continue;
      }
      if (in.op == OpCode::StoreAlpha) {
        ATX_TRY_VOID(store_requested(in, out, cells));
        continue;
      }
      (void)pool_.acquire();
      ATX_TRY_VOID(run_planned(prog, i, cache, dates, instruments, cells));
    }
    return atx::core::Ok(std::move(out));
  }

  [[nodiscard]] atx::core::Result<std::span<const atx::f64>>
  evaluate_root(const Program &prog, atx::u32 root, SubtreeCache *cache) {
    const atx::u32 one[1] = {root};
    ATX_TRY(SignalSet ss, evaluate_nodes(prog, std::span<const atx::u32>{one}, cache));
    root_buf_ = std::move(ss.alphas.front().values);
    return atx::core::Ok(std::span<const atx::f64>{root_buf_});
  }

  // The digest the cache keys this Engine's panel by. Computed lazily on first
  // cache use (O(cells*fields)); a caller that already has it (e.g. a pool of
  // Engines on one panel) injects it to skip the recomputation. PRECONDITION: the
  // injected value is panel_content_digest(panel) for THIS Engine's panel.
  void set_panel_digest(atx::u64 digest) noexcept { panel_digest_ = digest; }
  [[nodiscard]] atx::u64 panel_digest_value() {
    if (!panel_digest_.has_value()) {
      panel_digest_ = panel_content_digest(panel_);
    }
    return *panel_digest_;
  }

  // Cross-sectional DATE parallelism (Lane 2). When a non-null pool is set, every
  // Cs* op splits its date rows into contiguous bands across the pool's workers,
  // each band using that worker's private valid-set / group scratch. Rows are
  // independent (a date row reads only that date's rows) and each row's kernel
  // runs the same scan in the same order, so the result is bit-identical to the
  // serial loop for every pool size — AuditExact. Null (the DEFAULT) keeps the
  // original serial loop. Same DEADLOCK CONSTRAINT as set_ts_pool: never the pool
  // that is running this Engine's evaluate() as one of its jobs. It MAY be the same
  // pool passed to set_ts_pool (the two are never nested).
  void set_cs_pool(atx::engine::parallel::DetPool *pool) {
    cs_pool_ = pool;
    const atx::usize w = (pool != nullptr) ? pool->n_workers() : atx::usize{0};
    if (cs_valid_thr_.size() < w) {
      cs_valid_thr_.resize(w);
      cs_scratch_thr_.resize(w);
    }
  }
  [[nodiscard]] atx::engine::parallel::DetPool *cs_pool() const noexcept { return cs_pool_; }

  // =======================================================================
  //  Range execution (Lane 2 — strategy B). Executes ONE compute instruction over
  //  the sub-range [lo, hi) of its chunk_axis(), reading and writing the borrowed
  //  `slots` table (indexed by the instruction's SlotIds; a multi-output block
  //  occupies consecutive entries) instead of the Engine's own SlotPool.
  //  PRECONDITIONS: bind_fields(prog) ran for the Program the instruction came
  //  from; every read slot has `rd`, every written slot `wr`, all of length cells;
  //  hi <= axis length. Concurrent calls on DIFFERENT Engines over disjoint ranges
  //  (or different destination slots) are race-free: kernels write only their range.
  //  Errors: Err(InvalidArgument) for Free / StoreAlpha / an out-of-range slot.
  // =======================================================================
  [[nodiscard]] static ChunkAxis chunk_axis(OpCode op) noexcept {
    if (op == OpCode::LoadField || is_cs_op(op)) {
      return ChunkAxis::Dates;
    }
    if (is_instrument_op(op)) {
      return ChunkAxis::Instruments;
    }
    return ChunkAxis::Cells;
  }

  [[nodiscard]] atx::core::Status bind_fields(const Program &prog) { return resolve_fields(prog); }

  [[nodiscard]] atx::core::Status execute_range(const Instr &in, std::span<const ExtSlot> slots,
                                                atx::usize lo, atx::usize hi) {
    if (in.op == OpCode::Free || in.op == OpCode::StoreAlpha) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Engine::execute_range: Free/StoreAlpha are not compute instructions");
    }
    if (!slots_cover(in, slots)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Engine::execute_range: instruction slot outside the slot table");
    }
    ext_ = slots;
    const atx::core::Status s =
        dispatch_range(in, panel_.dates(), panel_.instruments(), lo, hi);
    ext_ = {};
    return s;
  }

  // Range execution of a FUSED kernel writing slot `dst` over cells [lo, hi) (the
  // fused counterpart of execute_range; same preconditions, Slot micro-op args and
  // `dst` index `slots`).
  [[nodiscard]] atx::core::Status execute_fused_range(const FusedKernel &k, SlotId dst,
                                                      std::span<const ExtSlot> slots, atx::usize lo,
                                                      atx::usize hi) {
    if (!fused_slots_cover(k, dst, slots.size())) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Engine::execute_fused_range: kernel slot outside the slot table");
    }
    ext_ = slots;
    const atx::core::Status s = eval_fused(k, dst, lo, hi);
    ext_ = {};
    return s;
  }

  // Evaluate a FusedProgram (alpha/fusion.hpp): the plain evaluate() loop, with each
  // fused sink running its blocked micro-program. Byte-identical to evaluating the
  // unfused source Program. Errors: every evaluate() error, plus Err(InvalidArgument)
  // when kernel_of does not align with the code or names a missing kernel.
  [[nodiscard]] atx::core::Result<SignalSet> evaluate(const FusedProgram &fp) {
    const Program &prog = fp.prog;
    if (fp.kernel_of.size() != prog.code.size()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Engine::evaluate(FusedProgram): kernel_of misaligned with code");
    }
    const atx::usize dates = panel_.dates();
    const atx::usize instruments = panel_.instruments();
    const atx::usize cells = dates * instruments;
    ATX_TRY_VOID(resolve_fields(prog));
    ensure_pool(prog.num_slots, cells);
    SignalSet out;
    out.dates = dates;
    out.instruments = instruments;
    out.alphas.resize(prog.roots.size());
    for (atx::usize r = 0; r < prog.roots.size(); ++r) {
      out.alphas[r].name = prog.roots[r].name;
      out.alphas[r].values.assign(cells, detail::kVmNaN);
    }
    for (atx::usize i = 0; i < prog.code.size(); ++i) {
      const Instr &in = prog.code[i];
      if (in.op == OpCode::Free) {
        pool_.release(in.dst);
        continue;
      }
      if (in.op == OpCode::StoreAlpha) {
        ATX_TRY_VOID(store_alpha(in, out, cells));
        continue;
      }
      (void)pool_.acquire();
      const atx::i32 kid = fp.kernel_of[i];
      if (kid >= 0) {
        if (static_cast<atx::usize>(kid) >= fp.kernels.size()) {
          return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                "Engine::evaluate(FusedProgram): kernel index out of range");
        }
        ATX_TRY_VOID(eval_fused(fp.kernels[static_cast<atx::usize>(kid)], in.dst, 0, cells));
        continue;
      }
      if (in.op == OpCode::Pin) {
        eval_pin(in, 0, cells);
        continue;
      }
      ATX_TRY_VOID(dispatch(in, dates, instruments, cells));
    }
    return atx::core::Ok(std::move(out));
  }

private:
  // Column access. Normally a Program SlotId indexes the pool buffer directly (see
  // evaluate()); under execute_range it indexes the borrowed ExtSlot table.
  [[nodiscard]] std::span<atx::f64> wcol(SlotId s) {
    if (!ext_.empty()) {
      ATX_ASSERT(s < ext_.size() && ext_[s].wr != nullptr);
      return std::span<atx::f64>{ext_[s].wr, ext_[s].n};
    }
    return pool_.column(s);
  }
  [[nodiscard]] std::span<const atx::f64> rcol(SlotId s) const {
    if (!ext_.empty()) {
      ATX_ASSERT(s < ext_.size() && ext_[s].rd != nullptr);
      return std::span<const atx::f64>{ext_[s].rd, ext_[s].n};
    }
    return pool_.column(s);
  }
  [[nodiscard]] std::span<atx::f64> dst_col(const Instr &in) { return wcol(in.dst); }
  [[nodiscard]] std::span<const atx::f64> src_col(const Instr &in, atx::usize k) const {
    return rcol(in.src.at(k));
  }

  [[nodiscard]] static bool is_cs_op(OpCode op) noexcept {
    switch (op) {
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
    case OpCode::CsBucket:  // W2: date rows (lit_ops.hpp)
    case OpCode::CsResidOn: // W2
    case OpCode::CsSumG:    // v8 YOPS group_sum
      return true;
    default:
      return false;
    }
  }

  // Ts*/OU rolling, recurrences and the Kalman record op: per-instrument columns.
  [[nodiscard]] static bool is_instrument_op(OpCode op) noexcept {
    const auto v = static_cast<atx::u8>(op);
    const bool ts_block = v >= static_cast<atx::u8>(OpCode::TsDelay) &&
                          v <= static_cast<atx::u8>(OpCode::OuFilter);
    return ts_block || op == OpCode::KalmanReg || op == OpCode::OuTheta ||
           op == OpCode::OuHalflife || op == OpCode::OuMean || op == OpCode::OuZscore ||
           detail::is_lit_ts_op(op); // W2 trailing-window ops
  }

  // Slot offset an operand reads beyond its own slot: Pin's projected pin, and a
  // W2 pack consumer's regressor block (width - 1; registry.hpp lit_reg_*).
  [[nodiscard]] static atx::usize operand_extent(const Instr &in, atx::usize k) noexcept {
    if (in.op == OpCode::Pin && k == 0) {
      return in.param;
    }
    if (detail::is_pack_consumer(in.op) && (k == 1 || k == 2)) {
      const atx::usize w =
          (k == 1) ? detail::lit_reg_wb(in.param) : detail::lit_reg_wc(in.param);
      return w > 1 ? w - 1 : atx::usize{0};
    }
    return 0;
  }

  [[nodiscard]] atx::usize axis_len(OpCode op, atx::usize dates, atx::usize instruments) const {
    switch (chunk_axis(op)) {
    case ChunkAxis::Cells:
      return dates * instruments;
    case ChunkAxis::Dates:
      return dates;
    case ChunkAxis::Instruments:
      return instruments;
    }
    ATX_UNREACHABLE();
  }

  [[nodiscard]] static bool slots_cover(const Instr &in, std::span<const ExtSlot> slots) noexcept {
    const atx::usize n = slots.size();
    if (static_cast<atx::usize>(in.dst) + in.n_out > n) {
      return false;
    }
    for (atx::usize k = 0; k < in.src.size(); ++k) {
      const SlotId s = in.src[k];
      if (s == kNoSlot) {
        continue;
      }
      const atx::usize extra = operand_extent(in, k);
      if (static_cast<atx::usize>(s) + extra >= n) {
        return false;
      }
    }
    return true;
  }

  // ---- Lane 2 planning (evaluate_nodes) -----------------------------------
  // want_pos_[root] = output position of a requested root, or kNotWanted.
  static constexpr atx::u32 kNotWanted = ~atx::u32{0};
  static constexpr atx::u32 kNoProducer = ~atx::u32{0};

  [[nodiscard]] atx::core::Status map_requested_roots(const Program &prog,
                                                      std::span<const atx::u32> roots) {
    want_pos_.assign(prog.roots.size(), kNotWanted);
    for (atx::usize k = 0; k < roots.size(); ++k) {
      if (roots[k] >= prog.roots.size() || want_pos_[roots[k]] != kNotWanted) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "Engine::evaluate_nodes: root index out of range or duplicated");
      }
      want_pos_[roots[k]] = static_cast<atx::u32>(k);
    }
    return atx::core::Ok();
  }

  // Forward pass: producer instruction of every operand; backward pass: exact
  // liveness from the requested StoreAlphas, stopping at cache hits.
  void plan_needed(const Program &prog, SubtreeCache *cache) {
    const atx::usize n = prog.code.size();
    slot_producer_.assign(static_cast<atx::usize>(prog.num_slots) + 1U, kNoProducer);
    producers_.assign(n, {kNoProducer, kNoProducer, kNoProducer});
    for (atx::usize i = 0; i < n; ++i) {
      const Instr &in = prog.code[i];
      if (in.op == OpCode::Free) {
        continue;
      }
      for (atx::usize k = 0; k < in.src.size(); ++k) {
        if (in.src[k] != kNoSlot) {
          producers_[i][k] = slot_producer_[in.src[k]];
        }
      }
      if (in.op != OpCode::StoreAlpha) {
        for (atx::usize k = 0; k < in.n_out; ++k) {
          slot_producer_[static_cast<atx::usize>(in.dst) + k] = static_cast<atx::u32>(i);
        }
      }
    }
    needed_.assign(n, 0);
    hits_.assign(n, nullptr);
    if (cache != nullptr) {
      subtree_hashes(prog, hashes_, slot_hash_scratch_);
    }
    const atx::u64 pdig = cache != nullptr ? panel_digest_value() : atx::u64{0};
    const atx::usize cells = panel_.cells();
    for (atx::usize ii = n; ii-- > 0;) {
      const Instr &in = prog.code[ii];
      if (in.op == OpCode::Free) {
        continue;
      }
      if (in.op == OpCode::StoreAlpha) {
        if (in.param < want_pos_.size() && want_pos_[in.param] != kNotWanted) {
          mark_needed(producers_[ii][0]);
        }
        continue;
      }
      if (needed_[ii] == 0) {
        continue;
      }
      if (cache != nullptr && subtree_cacheable(in)) {
        std::shared_ptr<const PanelBuf> hit = cache->find(SubtreeKey{hashes_[ii], pdig, mode_});
        if (hit != nullptr && hit->size() == cells) {
          hits_[ii] = std::move(hit);
          continue; // the cached value replaces this node's whole producer cone
        }
      }
      for (const atx::u32 p : producers_[ii]) {
        mark_needed(p);
      }
    }
  }

  void mark_needed(atx::u32 producer) noexcept {
    if (producer != kNoProducer) {
      needed_[producer] = 1;
    }
  }

  [[nodiscard]] atx::core::Status store_requested(const Instr &in, SignalSet &out,
                                                  atx::usize cells) {
    if (in.param >= want_pos_.size()) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "Engine::evaluate_nodes: StoreAlpha output index out of range");
    }
    const atx::u32 pos = want_pos_[in.param];
    if (pos == kNotWanted) {
      return atx::core::Ok();
    }
    const std::span<const atx::f64> src = src_col(in, 0);
    std::vector<atx::f64> &dst = out.alphas[pos].values;
    for (atx::usize i = 0; i < cells; ++i) {
      dst[i] = src[i];
    }
    return atx::core::Ok();
  }

  // Execute instruction i of a planned evaluation: skip if dead, copy if a cache
  // hit, else compute (and publish when a cache is attached and the op qualifies).
  [[nodiscard]] atx::core::Status run_planned(const Program &prog, atx::usize i,
                                              SubtreeCache *cache, atx::usize dates,
                                              atx::usize instruments, atx::usize cells) {
    const Instr &in = prog.code[i];
    if (needed_[i] == 0) {
      return atx::core::Ok();
    }
    if (hits_[i] != nullptr) {
      const std::span<atx::f64> dst = dst_col(in);
      const PanelBuf &src = *hits_[i];
      for (atx::usize c = 0; c < cells; ++c) {
        dst[c] = src[c];
      }
      hits_[i].reset(); // drop our reference promptly (the cache may evict it)
      return atx::core::Ok();
    }
    if (in.op == OpCode::Pin) {
      eval_pin(in, 0, cells);
      return atx::core::Ok();
    }
    ATX_TRY_VOID(dispatch(in, dates, instruments, cells));
    if (cache != nullptr && subtree_cacheable(in)) {
      const std::span<const atx::f64> v = src_view(in.dst);
      cache->publish(SubtreeKey{hashes_[i], panel_digest_value(), mode_},
                     PanelBuf(v.begin(), v.end()));
    }
    return atx::core::Ok();
  }

  [[nodiscard]] std::span<const atx::f64> src_view(SlotId s) const { return rcol(s); }

  [[nodiscard]] static bool fused_slots_cover(const FusedKernel &k, SlotId dst,
                                              atx::usize n) noexcept {
    if (static_cast<atx::usize>(dst) >= n || k.ops.empty()) {
      return false;
    }
    for (const MicroOp &m : k.ops) {
      if (m.kind == MicroKind::Slot && static_cast<atx::usize>(m.arg) >= n) {
        return false;
      }
    }
    return true;
  }

  // Run a fused micro-program over cells [lo, hi) in kFuseBlock-cell blocks.
  // Register r holds micro-op r's block; a Slot op ALIASES its source (no copy); the
  // last op writes straight into `dst`. Every Op applies detail::vm_apply_ew — the
  // same per-cell function the unfused instruction applies — and Field / Imm
  // reproduce LoadField / Const exactly, so the result is byte-identical.
  // SAFETY: operand registers always precede their consumer (fusion emits members
  // in topological order); kernels whose register file outgrows the scratch grow
  // it here once (cold), never inside the block loop.
  [[nodiscard]] atx::core::Status eval_fused(const FusedKernel &k, SlotId dst, atx::usize lo,
                                             atx::usize hi) {
    const atx::usize nops = k.ops.size();
    if (nops == 0 || k.ops.back().kind != MicroKind::Op) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Engine: fused kernel must end in an element-wise op");
    }
    if (fuse_regs_.size() < nops * kFuseBlock) {
      fuse_regs_.resize(nops * kFuseBlock);
    }
    if (fuse_ptr_.size() < nops) {
      fuse_ptr_.resize(nops);
    }
    const std::span<atx::f64> out = wcol(dst);
    for (atx::usize b = lo; b < hi; b += kFuseBlock) {
      const atx::usize len = std::min(kFuseBlock, hi - b);
      for (atx::usize r = 0; r < nops; ++r) {
        ATX_TRY_VOID(fused_step(k.ops[r], r, r + 1 == nops ? out.data() + b : nullptr, b, len));
      }
    }
    return atx::core::Ok();
  }

  // One micro-op over one block [b, b+len). `sink` (non-null only for the last op)
  // receives the result directly; otherwise register r's scratch does.
  [[nodiscard]] atx::core::Status fused_step(const MicroOp &m, atx::usize r, atx::f64 *sink,
                                             atx::usize b, atx::usize len) {
    atx::f64 *reg = sink != nullptr ? sink : fuse_regs_.data() + r * kFuseBlock;
    switch (m.kind) {
    case MicroKind::Slot:
      fuse_ptr_[r] = rcol(m.arg).data() + b;
      return atx::core::Ok();
    case MicroKind::Imm:
      for (atx::usize c = 0; c < len; ++c) {
        reg[c] = m.imm;
      }
      break;
    case MicroKind::Field: {
      if (m.arg >= field_remap_.size()) {
        return atx::core::Err(atx::core::ErrorCode::Internal,
                              "Engine: fused LoadField param out of field-dictionary range");
      }
      const std::span<const atx::f64> field = panel_.field_all(field_remap_[m.arg]);
      const atx::usize inst = panel_.instruments();
      atx::usize d = b / inst;
      atx::usize j = b % inst;
      for (atx::usize c = 0; c < len; ++c) {
        reg[c] = panel_.in_universe(d, j) ? field[b + c] : detail::kVmNaN;
        if (++j == inst) {
          j = 0;
          ++d;
        }
      }
      break;
    }
    case MicroKind::Op:
      detail::vm_apply_ew(m.op, fuse_ptr_[m.in[0]], fuse_ptr_[m.in[1]], fuse_ptr_[m.in[2]], reg,
                          len);
      break;
    }
    fuse_ptr_[r] = reg;
    return atx::core::Ok();
  }

  // Pin projects one output of its parent's contiguous block into its own single
  // slot: column(src[0] + param) -> column(dst), over cells [lo, hi).
  void eval_pin(const Instr &in, atx::usize lo, atx::usize hi) {
    const std::span<const atx::f64> src = rcol(in.src[0] + in.param);
    const std::span<atx::f64> dst_span = wcol(in.dst);
    for (atx::usize ci = lo; ci < hi; ++ci) {
      dst_span[ci] = src[ci];
    }
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
                                           atx::usize instruments, atx::usize /*cells*/) {
    return dispatch_range(in, dates, instruments, 0, axis_len(in.op, dates, instruments));
  }

  // The range form of dispatch: [lo, hi) along chunk_axis(in.op). The full range
  // is exactly the pre-Lane-2 serial kernel (same loops, same order).
  [[nodiscard]] atx::core::Status dispatch_range(const Instr &in, atx::usize dates,
                                                 atx::usize instruments, atx::usize lo,
                                                 atx::usize hi) {
    switch (in.op) {
    case OpCode::LoadField:
      return eval_load_field(in, instruments, lo, hi);
    case OpCode::Const:
      return eval_const(in, lo, hi);
    case OpCode::Add:
    case OpCode::Sub:
    case OpCode::Mul:
    case OpCode::Div:
    case OpCode::Pow:
    case OpCode::Spow:
    case OpCode::MinP:
    case OpCode::MaxP:
      return eval_elementwise(in, lo, hi);
    case OpCode::Neg:
    case OpCode::Abs:
    case OpCode::Sign:
    case OpCode::Log:
    case OpCode::Sigmoid:
    case OpCode::Tanh:
      return eval_elementwise(in, lo, hi);
    case OpCode::CmpLt:
    case OpCode::CmpGt:
    case OpCode::CmpLe:
    case OpCode::CmpGe:
    case OpCode::CmpEq:
    case OpCode::CmpNe:
      return eval_elementwise(in, lo, hi);
    case OpCode::And:
    case OpCode::Or:
      return eval_elementwise(in, lo, hi);
    case OpCode::Not:
      return eval_elementwise(in, lo, hi);
    case OpCode::Select:
      return eval_elementwise(in, lo, hi);
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
    case OpCode::CsSumG: // v8 YOPS group_sum (cs_ops.hpp)
      return eval_cross_section(in, instruments, lo, hi);
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
      return eval_time_series(in, dates, instruments, lo, hi);
    case OpCode::TradeWhen:
    case OpCode::Hump:
    case OpCode::KalmanLevel:
    case OpCode::OuFilter:
      return eval_recurrence(in, dates, instruments, lo, hi);
    case OpCode::Split2:
      return eval_split2(in, lo, hi);
    case OpCode::KalmanReg:
      return eval_kalman_reg(in, dates, instruments, lo, hi);
    case OpCode::Pin:
      eval_pin(in, lo, hi);
      return atx::core::Ok();
    // ---- platform-v7 W2 literature ops (lit_ops.hpp) ----
    case OpCode::ArgPack:
    case OpCode::GroupCross:
      return eval_lit_map(in, lo, hi);
    case OpCode::CsBucket:
    case OpCode::CsResidOn:
      return eval_lit_cs(in, instruments, lo, hi);
    case OpCode::TsTopkMean:
    case OpCode::TsResidOn:
    case OpCode::TsBetaOn:
    case OpCode::TsCountIncreases:
    case OpCode::TsSumMp:
    case OpCode::TsMeanMp:
    case OpCode::TsStdMp:
    case OpCode::TsZscoreMp:
    case OpCode::TsMinMp:
    case OpCode::TsMaxMp:
    case OpCode::TsDecayLinearMp:
    case OpCode::TsCorrMp:
      return eval_lit_ts(in, dates, instruments, lo, hi);
    case OpCode::StoreAlpha:
    case OpCode::Free:
      ATX_UNREACHABLE(); // StoreAlpha/Free handled by evaluate(); never dispatched
    }
    ATX_UNREACHABLE(); // exhaustive switch — no valid fallthrough
  }

  // ---- platform-v7 W2 literature ops (kernels + NaN rules: lit_ops.hpp) ------
  // pack2/pack3 copy their operands into the contiguous record block; group_cross
  // is element-wise. Cells [lo, hi).
  [[nodiscard]] atx::core::Status eval_lit_map(const Instr &in, atx::usize lo, atx::usize hi) {
    if (in.op == OpCode::ArgPack) {
      for (atx::u32 c = 0; c < in.n_out; ++c) {
        const std::span<const atx::f64> s = src_col(in, c);
        const std::span<atx::f64> o = wcol(in.dst + c);
        for (atx::usize i = lo; i < hi; ++i) {
          o[i] = s[i];
        }
      }
      return atx::core::Ok();
    }
    const std::span<const atx::f64> g1 = src_col(in, 0);
    const std::span<const atx::f64> g2 = src_col(in, 1);
    const std::span<atx::f64> o = dst_col(in);
    for (atx::usize i = lo; i < hi; ++i) {
      o[i] = detail::lit_group_cross(g1[i], g2[i]);
    }
    return atx::core::Ok();
  }

  // bucket / cs_resid_on over date rows [d0, d1): the same valid-set scan as
  // cs_one_date (non-NaN x AND the Cs eligibility mask, ascending index), serial.
  [[nodiscard]] atx::core::Status eval_lit_cs(const Instr &in, atx::usize instruments,
                                              atx::usize d0, atx::usize d1) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const std::span<atx::f64> out = dst_col(in);
    std::array<std::span<const atx::f64>, detail::kLitMaxReg> cov{};
    atx::usize k = 0;
    if (in.op == OpCode::CsResidOn) {
      const atx::usize wb = detail::lit_reg_wb(in.param);
      const atx::usize wc = detail::lit_reg_wc(in.param);
      for (atx::u32 c = 0; c < wb && k < cov.size(); ++c) {
        cov[k++] = rcol(in.src[1] + c);
      }
      for (atx::u32 c = 0; c < wc && k < cov.size(); ++c) {
        cov[k++] = rcol(in.src[2] + c);
      }
    }
    // SAFETY: analyze_lit_call proved bucket's n an integer in [2, 65535], so the
    // truncation to int is in range (imm[0] is 0.0 for cs_resid_on, unused).
    const int nb = static_cast<int>(in.imm[0]);
    std::array<std::span<const atx::f64>, detail::kLitMaxReg> crow{};
    const std::span<const atx::u8> mask{cs_mask_};
    for (atx::usize d = d0; d < d1; ++d) {
      const std::span<const atx::f64> xr = x.subspan(d * instruments, instruments);
      const std::span<atx::f64> orow = out.subspan(d * instruments, instruments);
      const std::span<const atx::u8> mrow =
          mask.empty() ? std::span<const atx::u8>{} : mask.subspan(d * instruments, instruments);
      cs_valid_.clear();
      for (atx::usize i = 0; i < instruments; ++i) {
        orow[i] = detail::kLitNaN;
        if (!std::isnan(xr[i]) && (mrow.empty() || mrow[i] != 0U)) {
          cs_valid_.push_back(i);
        }
      }
      if (in.op == OpCode::CsBucket) {
        detail::lit_bucket_row(xr, cs_valid_, nb, orow, cs_scratch_);
        continue;
      }
      for (atx::usize c = 0; c < k; ++c) {
        crow[c] = cov[c].subspan(d * instruments, instruments);
      }
      detail::lit_cs_resid_row(xr, std::span<const std::span<const atx::f64>>{crow.data(), k},
                               cs_valid_, orow, lit_scratch_);
    }
    return atx::core::Ok();
  }

  // W2 trailing-window ops over instrument columns [j0, j1): for each date the
  // window [max(0, t-d+1), t] of every input is gathered chronologically and the
  // shared cell kernel runs — the call StreamingEngine makes on its ring, so the
  // two agree bit-for-bit. Full-window ops short-circuit a short window to NaN
  // (the kernel would return NaN); the min-periods family evaluates it.
  [[nodiscard]] atx::core::Status eval_lit_ts(const Instr &in, atx::usize dates,
                                              atx::usize instruments, atx::usize j0,
                                              atx::usize j1) {
    atx::usize last = 0;
    for (atx::usize k = 0; k < in.src.size(); ++k) {
      if (in.src.at(k) != kNoSlot) {
        last = k;
      }
    }
    const atx::usize d = detail::tsv_window_of(src_col(in, last));
    const atx::usize n_in = detail::lit_ts_inputs(in.op, in.param);
    std::array<std::span<const atx::f64>, 1 + detail::kLitMaxReg> cols{};
    cols[0] = src_col(in, 0);
    if (in.op == OpCode::TsCorrMp) {
      cols[1] = src_col(in, 1);
    } else {
      for (atx::u32 c = 1; c < n_in && c < cols.size(); ++c) {
        cols[c] = rcol(in.src[1] + (c - 1U)); // the regressor block of operand 1
      }
    }
    const std::span<atx::f64> out = dst_col(in);
    const bool partial_ok = detail::is_lit_mp_op(in.op);
    if (lit_win_.size() < n_in * d) {
      lit_win_.resize(n_in * d);
    }
    for (atx::usize j = j0; j < j1; ++j) {
      for (atx::usize t = 0; t < dates; ++t) {
        const atx::usize len = std::min(t + 1, d);
        const atx::usize cell = t * instruments + j;
        if (d == 0 || (len < d && !partial_ok)) {
          out[cell] = detail::kLitNaN;
          continue;
        }
        const atx::usize first = t + 1 - len;
        for (atx::usize c = 0; c < n_in; ++c) {
          for (atx::usize i = 0; i < len; ++i) {
            lit_win_[c * len + i] = cols[c][(first + i) * instruments + j];
          }
        }
        out[cell] = detail::lit_ts_cell(in.op,
                                        std::span<const atx::f64>{lit_win_.data(), n_in * len},
                                        n_in, len, d, in.imm[0], lit_scratch_);
      }
    }
    return atx::core::Ok();
  }

  // ---- leaves -------------------------------------------------------------
  [[nodiscard]] atx::core::Status eval_const(const Instr &in, atx::usize lo, atx::usize hi) {
    const std::span<atx::f64> out = dst_col(in);
    for (atx::usize i = lo; i < hi; ++i) {
      out[i] = in.imm[0];
    }
    return atx::core::Ok();
  }

  // LoadField copies the field, NaN-ing any out-of-universe cell (point-in-time).
  // `in.param` indexes the program's field dictionary; remap to the Panel id.
  [[nodiscard]] atx::core::Status eval_load_field(const Instr &in, atx::usize instruments,
                                                  atx::usize d0, atx::usize d1) {
    if (in.param >= field_remap_.size()) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "Engine::evaluate: LoadField param out of field-dictionary range");
    }
    const std::span<atx::f64> out = dst_col(in);
    const std::span<const atx::f64> field = panel_.field_all(field_remap_[in.param]);
    for (atx::usize d = d0; d < d1; ++d) {
      for (atx::usize j = 0; j < instruments; ++j) {
        const atx::usize idx = d * instruments + j;
        out[idx] = panel_.in_universe(d, j) ? field[idx] : detail::kVmNaN;
      }
    }
    return atx::core::Ok();
  }

  // ---- element-wise (unary / binary / comparison / logical / not / select) --
  // All route through detail::vm_apply_ew — the single implementation the fused
  // kernels share — over the cell range [lo, hi).
  [[nodiscard]] atx::core::Status eval_elementwise(const Instr &in, atx::usize lo, atx::usize hi) {
    const atx::usize n = hi - lo;
    const atx::f64 *x = in.src[0] != kNoSlot ? src_col(in, 0).data() + lo : nullptr;
    const atx::f64 *y = in.src[1] != kNoSlot ? src_col(in, 1).data() + lo : nullptr;
    const atx::f64 *z = in.src[2] != kNoSlot ? src_col(in, 2).data() + lo : nullptr;
    detail::vm_apply_ew(in.op, x, y, z, dst_col(in).data() + lo, n);
    return atx::core::Ok();
  }

  // ---- cross-sectional (per date-row) -------------------------------------
  // Slice each whole-panel slot buffer to a single date row and apply the
  // cross-sectional kernel (cs_ops.hpp) over that row's VALID SET (non-NaN
  // cells). `x` is the input (src[0]); group ops take the classifier in
  // src[1], CsScale takes the scalar factor a == src[1][0]. EVERY output cell
  // is written (out-of-set -> NaN) since scratch slots are recycled. Mirrors
  // oracle.hpp's Oracle::eval_cross_section / cs_one_date dispatch exactly.
  [[nodiscard]] atx::core::Status eval_cross_section(const Instr &in, atx::usize instruments,
                                                     atx::usize d0, atx::usize d1) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const std::span<atx::f64> out = dst_col(in);
    const bool grouped =
        (in.op == OpCode::CsDemeanG || in.op == OpCode::CsNeutG || in.op == OpCode::CsRankG ||
         in.op == OpCode::CsZscoreG || in.op == OpCode::CsCountG || in.op == OpCode::CsMeanG ||
         in.op == OpCode::CsScaleG || in.op == OpCode::CsResidualize || in.op == OpCode::CsSumG);
    std::span<const atx::f64> g{};
    std::span<const atx::f64> z{}; // cs_residualize optional style covariate (src[2])
    // The scalar 2nd operand: CsScale's target L1 norm `a`, CsWinsorize's std
    // multiplier `k` or CsQuantile's bucket count. Read EXACTLY as CsScale does
    // (cell [0] of the slot). SAFETY (A-03): analyze() requires a finite Literal
    // in this slot (detail::validate_scalar_literal_operand), so the slot is a
    // Const broadcast and cell [0] IS the value — never a panel's first cell.
    atx::f64 scale_a = 1.0;
    if (grouped) {
      g = src_col(in, 1);
      if (in.op == OpCode::CsResidualize && in.src[2] != kNoSlot) {
        z = src_col(in, 2); // present only for the arity-3 cs_residualize(x, g, z)
      }
    } else if (in.op == OpCode::CsScale || in.op == OpCode::CsWinsorize ||
               in.op == OpCode::CsQuantile) {
      const std::span<const atx::f64> col = src_col(in, 1);
      scale_a = col.empty() ? detail::kVmNaN : col.front();
    }
    // Per-date valid-index scratch: an Engine member reused across dates AND across
    // evaluate() calls (grown once to peak, no per-instruction heap alloc). cs_one_date
    // clear()s it before each rebuild, so no stale entry is ever read -> byte-identical
    // to the previous fresh-per-call vector. The Engine is single-owner per worker, so
    // this member is touched by exactly one thread at a time (no cross-worker sharing).
    const CsRowsCtx ctx{in.op, x, g, z, out, scale_a, instruments, grouped,
                        policy_.rank_ties, cs_mask_};
    // Lane 2: date-band parallelism. Each band is a contiguous date range run by
    // ONE worker with ITS private valid/scratch, so every row executes the exact
    // serial row kernel — bit-identical for any band split or pool size.
    if (cs_pool_ != nullptr && d1 - d0 > 1) {
      const atx::usize n_rows = d1 - d0;
      const atx::usize bands = std::min(n_rows, cs_pool_->n_workers() * 4U);
      cs_pool_->parallel_for(bands, [this, &ctx, d0, n_rows, bands](atx::usize b,
                                                                    atx::usize wid) {
        const atx::usize lo = d0 + (n_rows * b) / bands;
        const atx::usize hi = d0 + (n_rows * (b + 1)) / bands;
        cs_rows(ctx, lo, hi, cs_valid_thr_[wid], cs_scratch_thr_[wid]);
      });
      return atx::core::Ok();
    }
    cs_rows(ctx, d0, d1, cs_valid_, cs_scratch_);
    return atx::core::Ok();
  }

  // Immutable per-op bundle for the Cs row loop (serial or banded).
  struct CsRowsCtx {
    OpCode op;
    std::span<const atx::f64> x;
    std::span<const atx::f64> g;
    std::span<const atx::f64> z;
    std::span<atx::f64> out;
    atx::f64 scale_a;
    atx::usize instruments;
    bool grouped;
    RankTies ties; // W0-A0 (A-01): rank-family tie policy
    std::span<const atx::u8> mask;
  };

  // Run the Cs row kernel for dates [d0, d1) with the caller's scratch.
  static void cs_rows(const CsRowsCtx &c, atx::usize d0, atx::usize d1,
                      std::vector<atx::usize> &valid, detail::CsScratch &scratch) {
    const atx::usize instruments = c.instruments;
    valid.reserve(instruments);
    for (atx::usize d = d0; d < d1; ++d) {
      const std::span<const atx::f64> xr = c.x.subspan(d * instruments, instruments);
      const std::span<atx::f64> orow = c.out.subspan(d * instruments, instruments);
      const std::span<const atx::f64> grow =
          c.grouped ? c.g.subspan(d * instruments, instruments) : std::span<const atx::f64>{};
      const std::span<const atx::f64> zrow =
          c.z.empty() ? std::span<const atx::f64>{} : c.z.subspan(d * instruments, instruments);
      const auto mask = c.mask.empty() ? std::span<const atx::u8>{}
                                       : c.mask.subspan(d * instruments, instruments);
      cs_one_date(c.op, xr, grow, zrow, c.scale_a, orow, valid, scratch, c.ties, mask);
    }
  }

  // Apply one cross-sectional op to a single date's row. `out` is reset to all
  // NaN here (out-of-set cells stay NaN — scratch slots are recycled), the
  // valid set is rebuilt into `valid` (caller-owned scratch), then dispatched.
  static void cs_one_date(OpCode op, std::span<const atx::f64> x, std::span<const atx::f64> g,
                          std::span<const atx::f64> z, atx::f64 scale_a, std::span<atx::f64> out,
                          std::vector<atx::usize> &valid, detail::CsScratch &scratch,
                          RankTies ties, std::span<const atx::u8> mask) {
    // INVARIANT (REQUIRED — not accidental): the forward scan produces `valid`
    // in strictly ascending instrument-index order, and every downstream kernel
    // depends on it for AuditExact-determinism:
    //   * cs_rank_row's stable sort breaks ties by this pre-sort order, so a
    //     non-ascending scan would silently flip tied-rank outputs;
    //   * the reduction kernels (cs_zscore_row's Σx / Σ(x-mean)², the grouped
    //     sums) accumulate in this scan order, and f64 addition is not
    //     associative — a permuted scan changes the summed bits in some cells.
    // Reordering worker dispatch or splitting the row therefore must NOT permute
    // this scan; ascending instrument index is the one canonical order.
    valid.clear();
    for (atx::usize i = 0; i < x.size(); ++i) {
      out[i] = detail::kVmNaN; // default every cell (out-of-set stays NaN)
      if (!detail::cs_is_nan(x[i]) && (mask.empty() || mask[i] != 0U)) {
        valid.push_back(i);
      }
    }
    switch (op) {
    case OpCode::CsRank:
      detail::cs_rank_row(x, valid, out, scratch, ties);
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
      detail::cs_group_demean_row(x, g, valid, out, scratch);
      break;
    case OpCode::CsResidualize: // demean (z empty) or FWL partial-out (z present)
      detail::cs_residualize_row(x, g, z, valid, out, scratch);
      break;
    case OpCode::CsQuantile: // discretize the valid set into `scale_a` buckets
      detail::cs_quantile_row(x, valid, scale_a, out, scratch, ties);
      break;
    case OpCode::CsVecSum:
      detail::cs_vec_reduce_row(x, valid, out, /*want_avg=*/false);
      break;
    case OpCode::CsVecAvg:
      detail::cs_vec_reduce_row(x, valid, out, /*want_avg=*/true);
      break;
    case OpCode::CsRankG:
      detail::cs_group_row(x, g, valid, out, /*zscore=*/false, scratch, ties);
      break;
    case OpCode::CsZscoreG:
      detail::cs_group_row(x, g, valid, out, /*zscore=*/true, scratch);
      break;
    case OpCode::CsCountG:
      detail::cs_group_count_mean_row(x, g, valid, out, /*want_mean=*/false, scratch);
      break;
    case OpCode::CsMeanG:
      detail::cs_group_count_mean_row(x, g, valid, out, /*want_mean=*/true, scratch);
      break;
    case OpCode::CsSumG:
      detail::cs_group_aggregate_row(x, g, valid, out, detail::GroupAgg::Sum, scratch);
      break;
    case OpCode::CsScaleG:
      detail::cs_group_scale_row(x, g, valid, out, scratch);
      break;
    default:
      ATX_UNREACHABLE();
    }
  }

  // ---- time-series (per instrument column, causal trailing window) ---------
  // Resolve the window `d` from the op's LAST operand, then fill every output
  // cell down each instrument column (STRIDED by `instruments`, trailing window
  // [t-d+1, t], iterate instrument-outer / date-inner to mirror the oracle's
  // loop nest). Two execution shapes (Task 7):
  //   * ONLINE sweep (ts_is_online_op): TsSum/Mean/Var/Std/Zscore/AvDiff carry a
  //     rolling Σx/Σx²/non-NaN-count down the column (O(T)); Min/Max/Scale carry
  //     a monotonic deque. The FP-sum ops are within a TIGHT TOLERANCE of the
  //     batch oracle (conformance proves it); min/max/scale stay bit-exact.
  //   * BATCH per-cell (every other Ts/OU/corr/cov op): ts_value_at / ts_pair_at
  //     / ou_value_at recompute over the full window, still bit-exact with the
  //     oracle. Mirrors oracle.hpp's eval_time_series structure.
  [[nodiscard]] atx::core::Status eval_time_series(const Instr &in, atx::usize dates,
                                                   atx::usize instruments, atx::usize j0,
                                                   atx::usize j1) {
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
    const bool binary_series =
        (in.op == OpCode::TsCorr || in.op == OpCode::TsCov || in.op == OpCode::TsRegression);
    const std::span<const atx::f64> y =
        binary_series ? src_col(in, 1) : std::span<const atx::f64>{};
    // OU rolling-fit ops (P3d-E4) fit AR(1) over the trailing window per cell;
    // they take the same windowed path but a distinct per-cell kernel.
    const bool ou_rolling = (in.op == OpCode::OuTheta || in.op == OpCode::OuHalflife ||
                             in.op == OpCode::OuMean || in.op == OpCode::OuZscore);
    // Lookback needs no window scratch or column transpose. Whole-width delay
    // copies one contiguous block; subranges and delta follow contiguous rows.
    if (in.op == OpCode::TsDelay || in.op == OpCode::TsDelta) {
      return eval_ts_lookback(in.op, x, out, dates, instruments, d, j0, j1);
    }

    // Reusable scratch sized to the window: NO per-cell allocation (grown only
    // when `d` exceeds any prior call). Only the batch sort/pair ops touch it.
    if (d > ts_scratch_a_.size()) {
      ts_scratch_a_.resize(d);
      ts_scratch_b_.resize(d);
    }
    // Online path (Task 7): rolling sweep down each instrument column. The deque
    // scratch is sized to `dates` (grown once); the sweep allocates nothing.
    // W0-A0 (A-13): under AuditExact + TsSumPath::WindowedV2 (the default)
    // TsSum/TsMean skip the online slide and fall through to the batch per-window
    // recompute below (oracle-exact, independent of the panel start); under
    // ResearchFast the slide is Neumaier-compensated. OnlineV1 is the pre-W0
    // uncompensated slide in every mode.
    const bool sum_op = (in.op == OpCode::TsSum || in.op == OpCode::TsMean);
    const bool legacy_sum = policy_.ts_sum == TsSumPath::OnlineV1;
    const bool windowed_sum = sum_op && !legacy_sum && mode_ == EvalMode::AuditExact;
    if (in.op == OpCode::TsDecayExp && mode_ == EvalMode::ResearchFast && policy_.is_default()) {
      if (d == 0 || d > dates) {
        for (atx::usize t = 0; t < dates; ++t)
          std::fill_n(out.data() + t * instruments + j0, j1 - j0, detail::kTsNaN);
        return atx::core::Ok();
      }
      ts_exp_coeff_.prepare(d, in.imm[0]);
      const atx::usize width = j1 - j0;
      const atx::usize tiles = width / detail::kTsInstrumentTile +
                              static_cast<atx::usize>(width % detail::kTsInstrumentTile != 0);
      const auto tile = [&](atx::usize i) {
        const atx::usize begin = j0 + i * detail::kTsInstrumentTile;
        const atx::usize end = begin + std::min(detail::kTsInstrumentTile, j1 - begin);
        sliding::sweep_exp_decay(x, out, dates, instruments, ts_exp_coeff_, begin, end);
      };
      if (ts_pool_ != nullptr && tiles > 1) {
        ts_pool_->parallel_for(tiles, [&](atx::usize i, atx::usize) { tile(i); });
      } else {
        for (atx::usize i = 0; i < tiles; ++i) tile(i);
      }
      return atx::core::Ok();
    }
    if (sum_op && !legacy_sum && mode_ == EvalMode::ResearchFast) {
      const atx::usize width = j1 - j0;
      const atx::usize tiles = width / detail::kTsInstrumentTile +
                              static_cast<atx::usize>(width % detail::kTsInstrumentTile != 0);
      const auto tile = [&](atx::usize i) {
        const atx::usize begin = j0 + i * detail::kTsInstrumentTile;
        const atx::usize end = begin + std::min(detail::kTsInstrumentTile, j1 - begin);
        detail::ts_sum_tile(in.op, x, out, dates, instruments, d, begin, end);
      };
      if (ts_pool_ != nullptr && tiles > 1) {
        ts_pool_->parallel_for(tiles, [&](atx::usize i, atx::usize) { tile(i); });
      } else {
        for (atx::usize i = 0; i < tiles; ++i) tile(i);
      }
      return atx::core::Ok();
    }
    if (binary_series && mode_ == EvalMode::ResearchFast && policy_.flat == FlatGuard::RelativeV2) {
      const atx::usize width = j1 - j0;
      const atx::usize tiles = width / detail::kTsInstrumentTile +
                              static_cast<atx::usize>(width % detail::kTsInstrumentTile != 0);
      const auto tile = [&](atx::usize i) {
        const atx::usize begin = j0 + i * detail::kTsInstrumentTile;
        const atx::usize end = begin + std::min(detail::kTsInstrumentTile, j1 - begin);
        sliding::sweep_comoment(in.op, x, y, out, dates, instruments, d, begin, end, true);
      };
      if (ts_pool_ != nullptr && tiles > 1) {
        ts_pool_->parallel_for(tiles, [&](atx::usize i, atx::usize) { tile(i); });
      } else {
        for (atx::usize i = 0; i < tiles; ++i) tile(i);
      }
      return atx::core::Ok();
    }
    if (detail::ts_is_online_op(in.op) && !windowed_sum) {
      const bool extreme =
          (in.op == OpCode::TsMin || in.op == OpCode::TsMax || in.op == OpCode::TsScale);
      if (extreme && dates > ts_dq_lo_.size()) {
        ts_dq_lo_.resize(dates);
        ts_dq_hi_.resize(dates);
      }
      for (atx::usize j = j0; j < j1; ++j) {
        if (extreme) {
          detail::ts_online_extreme(in.op, x, out, dates, j, d, instruments, ts_dq_lo_, ts_dq_hi_);
        } else {
          detail::ts_online_sum_family(in.op, x, out, dates, j, d, instruments,
                                       /*compensated=*/!legacy_sum);
        }
      }
      return atx::core::Ok();
    }
    // ResearchFast variance family (p7 S3-1/S3-2): TsVar/TsStd/TsZscore/TsAvDiff
    // route through the O(T) Welford/Neumaier online column sweep ONLY under
    // EvalMode::ResearchFast. Under AuditExact (the default) ts_is_online_variance_op
    // is never consulted, so the variance family falls through to the batch path
    // below — byte-identical to the oracle. The Welford sweep is per-instrument-
    // column and self-contained (no scratch), so it needs no column-extract.
    if (mode_ == EvalMode::ResearchFast && sliding::is_unary_sliding_op(in.op)) {
      const atx::usize width = j1 - j0;
      const atx::usize tiles = width / detail::kTsInstrumentTile +
                              static_cast<atx::usize>(width % detail::kTsInstrumentTile != 0);
      const auto tile = [&](atx::usize i) {
        const atx::usize begin = j0 + i * detail::kTsInstrumentTile;
        const atx::usize end = begin + std::min(detail::kTsInstrumentTile, j1 - begin);
        sliding::sweep_unary(in.op, x, out, dates, instruments, d, begin, end);
      };
      if (ts_pool_ != nullptr && tiles > 1) {
        ts_pool_->parallel_for(tiles, [&](atx::usize i, atx::usize) { tile(i); });
      } else {
        for (atx::usize i = 0; i < tiles; ++i) tile(i);
      }
      return atx::core::Ok();
    }
    if (mode_ == EvalMode::ResearchFast && detail::ts_is_online_variance_op(in.op)) {
      for (atx::usize j = j0; j < j1; ++j) {
        detail::tsv_welford_dispatch(in.op, x, out, dates, j, d, instruments);
      }
      return atx::core::Ok();
    }
    // S1-3: Column-extract transpose — extract instrument column j once into a
    // contiguous scratch buffer, then call the kernel with instruments=1, j=0.
    //
    // SAFETY (bit-exactness): each kernel element at (t,j) reads x[k*I+j] for
    // k in [t+1-d, t].  After extracting ts_col_[s] = x[s*I+j] for all s, the
    // kernel with instruments=1, j=0 reads col[k*1+0] = col[k] = x[k*I+j].
    // The chronological accumulation order k=t+1-d..t is UNCHANGED — every f64
    // multiply/add fires on the same operands in the same sequence.  The only
    // difference is memory layout; the arithmetic result is bit-for-bit identical.
    // tsv_gather, tsv_var, tsv_lin_fit, tsv_rank, and ou_ar1_fit all read
    // monotonically from buf[0..d-1] which maps to col[t+1-d..t] — same elements,
    // same order.  Bit-exact by construction.
    //
    // ts_scratch_a_ / ts_scratch_b_ retain their existing role (window-d scratch
    // for sort/gather/pair inside the kernel); ts_col_ / ts_col_b_ are the new
    // dates-sized column buffers, grown monotonically as Engine members.
    // S3-3: dispatch the instrument columns either serially (null pool, the
    // original loop — byte-for-byte unchanged) or across DetPool bands. Column
    // independence (disjoint output slots, per-band scratch) makes the parallel
    // result bit-identical to serial, so this stays AuditExact. The pool is used
    // only for instruments>1 (a single column has no work to split).
    const TsBatchCtx ctx{in, x, y, out, dates, instruments, d, binary_series, ou_rolling,
                         policy_.flat};
    if (ts_pool_ != nullptr && j1 - j0 > 1) {
      // Each index j is handled by exactly one worker `wid`; the body writes only
      // column j's output slots and reads only column j (+ shared read-only x/y),
      // using THIS worker's private scratch — no cross-thread shared mutable state.
      ts_pool_->parallel_for(j1 - j0, [this, &ctx, j0](atx::usize jj, atx::usize wid) {
        eval_ts_column(ctx, j0 + jj, ts_col_thr_[wid], ts_col_b_thr_[wid],
                       ts_scratch_a_thr_[wid], ts_scratch_b_thr_[wid]);
      });
      return atx::core::Ok();
    }
    if (dates > ts_col_.size()) {
      ts_col_.resize(dates);
      ts_col_b_.resize(dates);
    }
    for (atx::usize j = j0; j < j1; ++j) {
      eval_ts_column(ctx, j, ts_col_, ts_col_b_, ts_scratch_a_, ts_scratch_b_);
    }
    return atx::core::Ok();
  }

  // Each subrange owns [j0,j1) on every date. No neighboring worker's cells are
  // read as scratch or written. Full-width delay also supports overlapping
  // buffers: copy BEFORE filling the warmup prefix. Partial/delta overlap is
  // rejected explicitly; ordinary VM slots and strategy-B slots are disjoint.
  [[nodiscard]] atx::core::Status eval_ts_lookback(
      OpCode op, std::span<const atx::f64> x, std::span<atx::f64> out,
      atx::usize dates, atx::usize instruments, atx::usize d,
      atx::usize j0, atx::usize j1) const {
    if (j0 > j1 || j1 > instruments ||
        (instruments != 0 && dates > std::numeric_limits<atx::usize>::max() / instruments)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "time-series lookback: invalid geometry or range");
    }
    const atx::usize cells = dates * instruments;
    if (x.size() < cells || out.size() < cells ||
        cells > std::numeric_limits<atx::usize>::max() / sizeof(atx::f64)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "time-series lookback: invalid buffer size");
    }
    if (cells == 0 || j0 == j1) return atx::core::Ok();
    const bool block_delay = op == OpCode::TsDelay && j0 == 0 && j1 == instruments;
    const std::less<const atx::f64 *> less;
    const bool overlap = less(x.data(), out.data() + cells) &&
                         less(out.data(), x.data() + cells);
    if (!block_delay && overlap) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "time-series lookback: overlapping partial/delta buffers");
    }
    const atx::usize first = d == 0 ? dates : std::min(d, dates);
    if (block_delay) {
      if (first < dates) {
        std::memmove(out.data() + first * instruments, x.data(),
                     (dates - first) * instruments * sizeof(atx::f64));
      }
      std::fill_n(out.data(), first * instruments, detail::kTsNaN);
      return atx::core::Ok();
    }
    for (atx::usize t = 0; t < first; ++t) {
      std::fill_n(out.data() + t * instruments + j0, j1 - j0, detail::kTsNaN);
    }
    using Batch = xsimd::batch<atx::f64>;
    constexpr atx::usize tile_size = 64;
    for (atx::usize t = first; t < dates; ++t) {
      const atx::f64 *const current = x.data() + t * instruments;
      const atx::f64 *const prior = x.data() + (t - d) * instruments;
      atx::f64 *const dst = out.data() + t * instruments;
      if (op == OpCode::TsDelay) {
        std::memcpy(dst + j0, prior + j0, (j1 - j0) * sizeof(atx::f64));
        continue;
      }
      for (atx::usize begin = j0; begin < j1;) {
        const atx::usize end = begin + std::min(tile_size, j1 - begin);
        atx::usize j = begin;
        if (mode_ == EvalMode::ResearchFast) {
          for (; end - j >= Batch::size; j += Batch::size) {
            (Batch::load_unaligned(current + j) - Batch::load_unaligned(prior + j))
                .store_unaligned(dst + j);
          }
        }
        for (; j < end; ++j) dst[j] = current[j] - prior[j];
        begin = end;
      }
    }
    return atx::core::Ok();
  }

  // Immutable per-evaluate context shared by every instrument column of a single
  // batch Ts op (so the column body — serial or parallel — reads one bundle).
  struct TsBatchCtx {
    const Instr &in;
    std::span<const atx::f64> x;
    std::span<const atx::f64> y; // empty unless binary_series
    std::span<atx::f64> out;
    atx::usize dates;
    atx::usize instruments;
    atx::usize d;
    bool binary_series;
    bool ou_rolling;
    FlatGuard flat; // W0-A0 (A-09): flat-window guard policy
  };

  // Evaluate ONE instrument column `j` of a batch Ts op into ctx.out, using the
  // caller-provided scratch (the serial Engine buffers, or this worker's private
  // per-thread buffers under S3-3 column parallelism). `col` / `col_b` are the
  // dates-sized column extracts; `sa` / `sb` are the window-d sort/pair scratch.
  //
  // SAFETY (bit-exactness + thread-safety): identical arithmetic to the original
  // serial loop — extract column j (strided), then call the kernel with
  // instruments=1, j=0 so col[k] == x[k*I+j] in the SAME chronological order. The
  // only reads outside this column are the read-only x/y spans; the only writes are
  // ctx.out[t*I+j] for this j. Distinct j -> disjoint output slots, so concurrent
  // columns never collide. Each scratch buffer belongs to exactly one caller.
  void eval_ts_column(const TsBatchCtx &ctx, atx::usize j, std::vector<atx::f64> &col,
                      std::vector<atx::f64> &col_b, std::vector<atx::f64> &sa,
                      std::vector<atx::f64> &sb) {
    const atx::usize dates = ctx.dates;
    const atx::usize instruments = ctx.instruments;
    const atx::usize d = ctx.d;
    // Grow-only resize of THIS caller's scratch (no shared-vector mutation under
    // parallelism: each worker owns its own col/col_b/sa/sb).
    if (col.size() < dates) {
      col.resize(dates);
      col_b.resize(dates);
    }
    if (sa.size() < d) {
      sa.resize(d);
      sb.resize(d);
    }
    for (atx::usize s = 0; s < dates; ++s) {
      col[s] = ctx.x[s * instruments + j];
    }
    const std::span<const atx::f64> cspan{col.data(), dates};
    if (ctx.binary_series) {
      for (atx::usize s = 0; s < dates; ++s) {
        col_b[s] = ctx.y[s * instruments + j];
      }
      const std::span<const atx::f64> cb{col_b.data(), dates};
      for (atx::usize t = 0; t < dates; ++t) {
        ctx.out[t * instruments + j] =
            detail::ts_pair_at(ctx.in.op, cspan, cb, t, 0, d, 1, sa, sb, ctx.flat);
      }
    } else if (ctx.ou_rolling) {
      for (atx::usize t = 0; t < dates; ++t) {
        ctx.out[t * instruments + j] = detail::ou_value_at(ctx.in.op, cspan, t, 0, d, 1, sa);
      }
    } else {
      for (atx::usize t = 0; t < dates; ++t) {
        ctx.out[t * instruments + j] =
            detail::ts_value_at(ctx.in.op, cspan, t, 0, d, 1, sa, ctx.in.imm[0], ctx.flat);
      }
    }
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
                                                  atx::usize instruments, atx::usize j0,
                                                  atx::usize j1) {
    const std::span<atx::f64> out = dst_col(in);
    if (in.op == OpCode::KalmanLevel) {
      return eval_kalman_level(in, out, dates, instruments, j0, j1);
    }
    if (in.op == OpCode::OuFilter) {
      return eval_ou_filter(in, out, dates, instruments, j0, j1);
    }
    if (instruments > state_.size()) {
      state_.resize(instruments); // grow-once; reused across calls
    }
    if (in.op == OpCode::Hump) {
      const std::span<const atx::f64> x = src_col(in, 0);
      // SAFETY (A-03): analyze() requires a finite Literal threshold, so src[1]
      // is a Const broadcast and cell [0] is the threshold itself.
      const atx::f64 thr = in.src.at(1) == kNoSlot ? atx::f64{0.01} : src_col(in, 1).front();
      if (policy_.hump == HumpNaN::StickyV1) {
        for (atx::usize j = j0; j < j1; ++j) {
          for (atx::usize t = 0; t < dates; ++t) {
            const atx::usize i = t * instruments + j;
            const atx::f64 v = detail::hump_step_v1(state_[j], x[i], thr, /*first=*/t == 0);
            out[i] = v;
            state_[j] = v;
          }
        }
        return atx::core::Ok();
      }
      // W0-A0 (A-02): per-instrument HumpState (prior + NaN-run length) lives on
      // the stack for its column's forward scan — the column is independent.
      for (atx::usize j = j0; j < j1; ++j) {
        detail::HumpState s{};
        for (atx::usize t = 0; t < dates; ++t) {
          const atx::usize i = t * instruments + j;
          out[i] = detail::hump_step(s, x[i], thr);
        }
      }
      return atx::core::Ok();
    }
    // TradeWhen: trigger=src[0], alpha=src[1], exit=src[2].
    const std::span<const atx::f64> trig = src_col(in, 0);
    const std::span<const atx::f64> alpha = src_col(in, 1);
    const std::span<const atx::f64> exit_v = src_col(in, 2);
    for (atx::usize j = j0; j < j1; ++j) {
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
                                                    atx::usize dates, atx::usize instruments,
                                                    atx::usize j0, atx::usize j1) {
    const std::span<const atx::f64> z = src_col(in, 0);
    const atx::f64 Q = in.imm[0];
    const atx::f64 R = in.imm[1];
    for (atx::usize j = j0; j < j1; ++j) {
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
                                                 atx::usize dates, atx::usize instruments,
                                                 atx::usize j0, atx::usize j1) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const atx::f64 theta = in.imm[0];
    const atx::f64 mu = in.imm[1];
    for (atx::usize j = j0; j < j1; ++j) {
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
  [[nodiscard]] atx::core::Status eval_split2(const Instr &in, atx::usize c0, atx::usize c1) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const std::span<atx::f64> hi = wcol(in.dst + 0);
    const std::span<atx::f64> lo = wcol(in.dst + 1);
    for (atx::usize i = c0; i < c1; ++i) {
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
                                                  atx::usize instruments, atx::usize j0,
                                                  atx::usize j1) {
    const std::span<const atx::f64> y = src_col(in, 0);
    const std::span<const atx::f64> x = src_col(in, 1);
    const atx::f64 delta = in.imm[0];
    const atx::f64 R = in.imm[1];
    const std::span<atx::f64> oa = wcol(in.dst + 0);
    const std::span<atx::f64> ob = wcol(in.dst + 1);
    const std::span<atx::f64> orr = wcol(in.dst + 2);
    for (atx::usize j = j0; j < j1; ++j) {
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
  EvalMode mode_{EvalMode::AuditExact}; // determinism tier (p7 S3-1); default inert
  KernelPolicy policy_{};               // W0-A0 versioned kernel policies; default corrected
  std::vector<atx::u8> cs_mask_;        // owned Cs* eligibility; empty preserves the default
  SlotPool pool_{1, 1};                // reused across calls; grown on demand
  std::vector<FieldId> field_remap_;   // program field id -> Panel FieldId scratch
  std::vector<atx::f64> ts_scratch_a_; // Ts* window scratch (sort/corr/cov); grown on demand
  std::vector<atx::f64> ts_scratch_b_; // Ts* second-window scratch (corr/cov)
  sliding::ExpDecayCoefficients ts_exp_coeff_; // grow-only, immutable during tile dispatch
  // S1-3: per-instrument column-extract buffers (dates-sized).  Grown
  // monotonically; never allocated inside the (t,j) hot loop.  ts_col_ holds the
  // extracted x column; ts_col_b_ the y column for binary ops.
  std::vector<atx::f64> ts_col_;       // Ts* column-extract scratch for x; size=dates
  std::vector<atx::f64> ts_col_b_;     // Ts* column-extract scratch for y (binary ops)
  std::vector<atx::usize> ts_dq_lo_;   // Ts online min/scale monotonic deque (date indices)
  std::vector<atx::usize> ts_dq_hi_;   // Ts online max/scale monotonic deque (date indices)
  std::vector<atx::f64> state_;        // recurrence state[n_instruments]; grown once, reused
  std::vector<atx::usize> cs_valid_;   // Cs* per-date valid-index scratch; grown once, cleared per date
  detail::CsScratch cs_scratch_;       // Cs* grouped/sort scratch; grown once, reset per date
  std::vector<atx::f64> lit_win_;      // W2 Ts gathered windows (n_in * d); grown on demand
  detail::LitScratch lit_scratch_;     // W2 sort / OLS rows / regression-row scratch
  // S3-3: optional intra-eval column pool + PER-WORKER scratch. ts_pool_ is null by
  // default (serial). When set, the batch column loop runs over instrument bands on
  // the pool; each worker `wid` owns ts_col_thr_[wid] (x column), ts_col_b_thr_[wid]
  // (y column), ts_scratch_a_thr_[wid] / ts_scratch_b_thr_[wid] (window-d sort/pair
  // scratch) so no two threads touch shared mutable state. All four are sized to
  // [n_workers] up front in set_ts_pool() and grown lazily inside the band body
  // (same growth-only pattern as the serial buffers), so there is no growth race.
  atx::engine::parallel::DetPool *ts_pool_{nullptr}; // borrowed; null = single-threaded
  std::vector<std::vector<atx::f64>> ts_col_thr_;       // per-worker x column-extract (dates)
  std::vector<std::vector<atx::f64>> ts_col_b_thr_;     // per-worker y column-extract (dates)
  std::vector<std::vector<atx::f64>> ts_scratch_a_thr_; // per-worker window scratch a (d)
  std::vector<std::vector<atx::f64>> ts_scratch_b_thr_; // per-worker window scratch b (d)
  // Lane 2: optional Cs date-band pool + per-worker valid/scratch (sized in set_cs_pool).
  atx::engine::parallel::DetPool *cs_pool_{nullptr}; // borrowed; null = serial rows
  std::vector<std::vector<atx::usize>> cs_valid_thr_;
  std::vector<detail::CsScratch> cs_scratch_thr_;
  // Lane 2: execute_range's borrowed slot table (empty outside execute_range).
  std::span<const ExtSlot> ext_{};
  // Lane 2: evaluate_nodes planning scratch (grown monotonically).
  std::vector<atx::u32> all_roots_;
  std::vector<atx::u32> want_pos_;
  std::vector<atx::u32> slot_producer_;
  std::vector<std::array<atx::u32, 3>> producers_;
  std::vector<atx::u8> needed_;
  std::vector<std::shared_ptr<const PanelBuf>> hits_;
  std::vector<SubtreeHash> hashes_;
  std::vector<SubtreeHash> slot_hash_scratch_;
  std::vector<atx::f64> root_buf_; // evaluate_root's output (valid until the next call)
  std::optional<atx::u64> panel_digest_;
  // Lane 2: fused-kernel register file (nops * kFuseBlock) + per-op block pointers.
  std::vector<atx::f64> fuse_regs_;
  std::vector<const atx::f64 *> fuse_ptr_;
};

} // namespace atx::engine::alpha
