#pragma once

// atx::engine::alpha — semantic analysis: shape/dtype + lookback (P3-3).
//
// Given a fully-parsed `Ast`, `analyze` walks each root subtree post-order and
// computes, per node, a `TypeInfo { Shape, DType, lookback }`:
//
//   * Shape  — the broadcast lattice (Scalar < CrossSection < Panel). Each op
//              has a table-driven shape rule (registry's shape_of); operand
//              shape mismatches are rejected (e.g. a Cs*/Ts* op fed a pure
//              scalar primary operand).
//   * DType  — F64 / Mask / Group. Comparisons & logicals yield Mask; the four
//              group-aware cross-sectional ops require a Group classifier as
//              their 2nd argument; SELECT requires a Mask condition; etc.
//   * lookback — the causality rail: how many *prior* bars a value at date `t`
//              depends on. This is the load-bearing safety property: an alpha
//              with lookback `L` must not be evaluated before `L` bars of
//              warm-up exist, or it reads uninitialised history. Two temporal
//              families:
//                - shift   (delay/delta): lookback = d + max(child)
//                - rolling (every other Ts*): lookback = (d-1) + max(child)
//              non-temporal ops add 0 (cross-sectional reduce within one date).
//
// Errors travel in the return type (`Result<Analysis>`); nothing throws. Type
// and shape violations map to `ErrorCode::InvalidArgument`.
//
// Header-only; every free function is `inline` (ODR). Analysis is a COLD path
// (run once per compiled alpha), so std::vector allocation is fine.
//
// Ownership / lifetime:
//   * `analyze` borrows the `Ast` by const ref for the duration of the call and
//     copies out an owning `Analysis`. The Analysis does not alias the Ast.
//   * Window-argument values are read from already-folded Literal nodes; the
//     parser guarantees constant subtrees are folded, so a non-Literal window
//     is a genuine "non-constant window" error, not a missed fold.

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"

namespace atx::engine::alpha {

// =========================================================================
//  TypeInfo — the per-node analysis result (parallel to Ast::nodes()).
// =========================================================================

struct TypeInfo {
  Shape shape{Shape::Scalar};
  DType dtype{DType::F64};
  atx::u16 lookback{}; // prior bars required at date t (the causality rail)
  bool is_record{false};
  std::span<const PinSig> pins{}; // valid iff is_record
};

namespace detail {

// ----- opcode family predicates -----------------------------------------

// The shift temporal family (lookback = d + max(child)): these reference the
// value `d` bars ago without a window reduction.
[[nodiscard]] inline bool is_shift_ts(OpCode op) noexcept {
  return op == OpCode::TsDelay || op == OpCode::TsDelta;
}

// The rolling-window temporal family (lookback = (d-1) + max(child)): every
// time-series op that reduces over a trailing window of `d` bars. EMA/WMA are
// treated as rolling here — they consume a `d`-bar window like the rest, so
// `(d-1)` is the correct minimum warm-up (an EMA seeded over `d` bars).
[[nodiscard]] bool is_rolling_ts(OpCode op) noexcept;

// True if `op` is any time-series op (shift or rolling) — these all return a
// Panel and require a non-scalar primary operand.
[[nodiscard]] inline bool is_time_series(OpCode op) noexcept {
  return is_shift_ts(op) || is_rolling_ts(op);
}

// True if `op` is a cross-sectional op (the Cs* family). These reduce within a
// single date (lookback += 0) and require a non-scalar primary operand.
[[nodiscard]] inline bool is_cross_section(OpCode op) noexcept {
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
    return true;
  default:
    return false;
  }
}

// The group-aware cross-sectional ops: their 2nd argument must carry a Group
// classifier dtype (the four neutralize/rank/zscore variants + the P3b-2
// group aggregates group_count/group_mean/group_scale + S3.1 cs_residualize).
[[nodiscard]] inline bool needs_group_arg(OpCode op) noexcept {
  return op == OpCode::CsDemeanG || op == OpCode::CsNeutG || op == OpCode::CsRankG ||
         op == OpCode::CsZscoreG || op == OpCode::CsCountG || op == OpCode::CsMeanG ||
         op == OpCode::CsScaleG || op == OpCode::CsResidualize;
}

// ----- field classification ---------------------------------------------

// A field is a Group classifier iff its name carries the `IndClass.` prefix
// (IndClass.sector / .industry / .subindustry), OR if it is the bare canonical
// gics-derived "sector" field (kHistFieldSector). All other fields are F64.
[[nodiscard]] inline bool is_group_field(std::string_view name) noexcept {
  constexpr std::string_view kPrefix = "IndClass.";
  if (name == "sector") return true;              // gics-derived classifier column
  return name.size() > kPrefix.size() && name.substr(0, kPrefix.size()) == kPrefix;
}

// ----- window-argument validation ----------------------------------------

// Read & validate a temporal op's window argument. The window is the LAST arg:
// arg `c` for arity-3 (corr/cov), else arg `b`. It MUST be a folded Literal
// (non-constant window → error) holding a finite positive value. A non-integer
// positive literal is FLOORED (P3b-4 §6 lock #3): the 101-alphas paper mines
// fractional window constants (e.g. ts_mean(close, 8.7)), and the canonical
// convention is floor(d). After flooring, a window of 0 (e.g. 0.5 → 0), any
// d <= 0, and a window above u16::max are all rejected. Returns the floored
// window as u16 on success.
[[nodiscard]] atx::core::Result<atx::u16> window_value(const Ast &ast, const Expr &call);

// ----- per-kind analyzers (each builds the node's TypeInfo or an error) ----

// Lookback of a node's children: max over the populated child slots. Element-
// wise/cross-sectional ops add 0 on top of this; temporal ops add their window.
[[nodiscard]] inline atx::u16 max_child_lookback(std::span<const TypeInfo> out, const Expr &e) {
  atx::u16 m = 0;
  const std::array<ExprId, 3> kids{e.a, e.b, e.c};
  for (const ExprId id : kids) {
    if (id != kNoExpr) {
      m = (out[id].lookback > m) ? out[id].lookback : m;
    }
  }
  return m;
}

// Unary node: shape follows the child; dtype is Mask for Not else F64, with
// operand-dtype validation per the op.
[[nodiscard]] atx::core::Result<TypeInfo> analyze_unary(std::span<const TypeInfo> out,
                                                        const Expr &e);

// Binary node: comparisons/logicals → Mask (with operand checks); arithmetic →
// F64 (both operands numeric). Shape is the broadcast-max of the two operands.
[[nodiscard]] atx::core::Result<TypeInfo> analyze_binary(std::span<const TypeInfo> out,
                                                         const Expr &e);

// Select (desugared ternary): cond must be Mask; then/else must be F64; result
// shape is the broadcast-max over ALL three operands. The condition DOES widen
// the result: `(close > open) ? 1 : 0` has scalar branches but a panel mask, so
// the per-cell select yields a Panel — dropping cond would mis-size the slot.
[[nodiscard]] atx::core::Result<TypeInfo> analyze_select(std::span<const TypeInfo> out,
                                                         const Expr &e);

// Guard: reject any operand that is a raw record value (must project with .pin
// before use in arithmetic, calls, or control flow). Returns Ok on success or
// Err(InvalidArgument) for the first offending operand.
[[nodiscard]] inline atx::core::Status reject_record_operands(std::span<const TypeInfo> out,
                                                              const Expr &e) {
  const std::array<ExprId, 3> kids{e.a, e.b, e.c};
  for (const ExprId id : kids) {
    if (id != kNoExpr && out[id].is_record) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "record value must be projected with .pin before use");
    }
  }
  return atx::core::Ok();
}

// Validate dtype constraints for the special-cased stateful ops (trade_when and
// hump). Called only when `op` is one of those two; returns Ok otherwise.
[[nodiscard]] atx::core::Status validate_stateful_op_dtypes(OpCode op, std::span<const TypeInfo> out,
                                                            const Expr &e);

// Validate the per-op hyperparameter RANGE constraints for the filter
// recurrence ops (hparams are already verified finite by analyze_call's
// finite-constant loop). Only the filter ops carry range rails; every other op
// has no hparam ranges, so a `default: break` is correct here (this is NOT an
// exhaustive-over-all-OpCodes switch — it only handles the filter ops).
[[nodiscard]] atx::core::Status validate_hparam_ranges(OpCode op, const Expr &e);

// Validate a Call node's STRUCTURE against its op's declared contract (S3.4):
// the peeled-hparam count matches `op->n_hparams`, and the number of
// materialized operand slots (a/b/c) lies in the op's [operand_min, operand_max]
// range. This is the load-bearing rail that makes "analyze-valid ⟹ VM-safe" hold
// for EVERY mutation/crossover, not just parser output — it rejects a swapped
// node that, e.g., carries a finite-default op (scale/winsorize/quantile, whose
// kernel unconditionally reads operand 2) without having materialized operand 2.
// Precondition: `e.kind == Call` and `e.op != nullptr`.
[[nodiscard]] atx::core::Status validate_node_contract(const Expr &e);

// Call node: shape from the op's table-driven rule, dtype from the registry row
// (+ group-arg validation), lookback from the temporal family. Cs*/Ts* ops
// reject a pure-scalar primary operand.
[[nodiscard]] atx::core::Result<TypeInfo> analyze_call(const Ast &ast,
                                                       std::span<const TypeInfo> out, const Expr &e);

// Member node: pin projection from a record-valued operand. Resolves the named
// pin in the record's PinSig table and returns its dtype with the record's
// shape/lookback. Rejects non-record operands and unknown pin names.
[[nodiscard]] atx::core::Result<TypeInfo> analyze_member(std::span<const TypeInfo> out,
                                                         const Ast &ast, const Expr &e);

// Dispatch one node to its kind-specific analyzer. `out` holds the already-
// computed TypeInfo of every node with a smaller ExprId (children come first).
[[nodiscard]] atx::core::Result<TypeInfo> analyze_node(const Ast &ast,
                                                       std::span<const TypeInfo> out, const Expr &e);

} // namespace detail

// =========================================================================
//  Analysis — the analyze() output (owns per-node TypeInfo + the program's
//  required lookback). Indexed by ExprId, parallel to Ast::nodes().
// =========================================================================

class Analysis {
public:
  [[nodiscard]] std::span<const TypeInfo> nodes() const noexcept { return nodes_; }
  [[nodiscard]] const TypeInfo &info(ExprId id) const noexcept { return nodes_[id]; }

  // The maximum lookback over every root — the warm-up the program needs.
  [[nodiscard]] atx::u16 required_lookback() const noexcept { return required_lookback_; }

  // Builder API (populated by analyze).
  void reserve(atx::usize n) { nodes_.reserve(n); }
  void push(const TypeInfo &t) { nodes_.push_back(t); }
  void set_required_lookback(atx::u16 lb) noexcept { required_lookback_ = lb; }

private:
  std::vector<TypeInfo> nodes_;  // parallel to Ast::nodes(), by ExprId
  atx::u16 required_lookback_{}; // max over all roots
};

// =========================================================================
//  Public API
// =========================================================================

// Analyze a fully-parsed `Ast`: compute shape/dtype/lookback for every node and
// validate the type lattice & causality rails. Returns the populated Analysis
// or the first violation as Err(InvalidArgument).
//
// Implementation note: the parser appends children before parents, so the arena
// is already in topological (post-)order — node `i` only references nodes with
// smaller ids. We exploit that to fill `nodes_` in a single forward pass; no
// recursion, so the walk is trivially bounded by the arena size.
[[nodiscard]] atx::core::Result<Analysis> analyze(const Ast &ast);

} // namespace atx::engine::alpha
