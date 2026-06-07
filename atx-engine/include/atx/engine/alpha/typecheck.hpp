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
#include <cmath>
#include <limits>
#include <span>
#include <string>
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
[[nodiscard]] inline bool is_rolling_ts(OpCode op) noexcept {
  switch (op) {
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
    return true;
  // Not rolling-window time-series ops.
  case OpCode::TsDelay:
  case OpCode::TsDelta:
  case OpCode::LoadField:
  case OpCode::Const:
  case OpCode::Add:
  case OpCode::Sub:
  case OpCode::Mul:
  case OpCode::Div:
  case OpCode::Neg:
  case OpCode::Abs:
  case OpCode::Sign:
  case OpCode::Log:
  case OpCode::Sigmoid:
  case OpCode::Tanh:
  case OpCode::Pow:
  case OpCode::Spow:
  case OpCode::MinP:
  case OpCode::MaxP:
  case OpCode::CmpLt:
  case OpCode::CmpGt:
  case OpCode::CmpLe:
  case OpCode::CmpGe:
  case OpCode::CmpEq:
  case OpCode::CmpNe:
  case OpCode::And:
  case OpCode::Or:
  case OpCode::Not:
  case OpCode::Select:
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
  case OpCode::TradeWhen:
  case OpCode::Hump:
  case OpCode::Pin:
  case OpCode::Split2:
  case OpCode::StoreAlpha:
  case OpCode::Free:
    return false;
  }
  return false; // unreachable for valid OpCode
}

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
    return true;
  default:
    return false;
  }
}

// The group-aware cross-sectional ops: their 2nd argument must carry a Group
// classifier dtype (the four neutralize/rank/zscore variants + the P3b-2
// group aggregates group_count/group_mean/group_scale).
[[nodiscard]] inline bool needs_group_arg(OpCode op) noexcept {
  return op == OpCode::CsDemeanG || op == OpCode::CsNeutG || op == OpCode::CsRankG ||
         op == OpCode::CsZscoreG || op == OpCode::CsCountG || op == OpCode::CsMeanG ||
         op == OpCode::CsScaleG;
}

// ----- field classification ---------------------------------------------

// A field is a Group classifier iff its name carries the `IndClass.` prefix
// (IndClass.sector / .industry / .subindustry). All other fields are F64.
[[nodiscard]] inline bool is_group_field(std::string_view name) noexcept {
  constexpr std::string_view kPrefix = "IndClass.";
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
[[nodiscard]] inline atx::core::Result<atx::u16> window_value(const Ast &ast, const Expr &call) {
  const ExprId window_id = (call_arity(call) == 3) ? call.c : call.b;
  const Expr &w = ast.node(window_id);
  if (w.kind != Expr::Kind::Literal) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "window must be a compile-time constant");
  }
  const atx::f64 v = w.value;
  if (!std::isfinite(v)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "window must be a finite positive number");
  }
  // Floor a fractional positive literal rather than reject it (lock #3): the
  // floor is the paper's window convention; the <=0 rail survives because a
  // non-positive or sub-1 literal (e.g. 0.5, -3) floors to <= 0 and is rejected
  // below. The non-constant rail is untouched (handled above).
  const atx::f64 floored = std::floor(v);
  if (floored < 1.0) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "window must floor to a positive integer (>= 1)");
  }
  // Upper-bound rail: the result is cast to u16, and a float→int conversion
  // whose truncated value is outside the destination range is UNDEFINED
  // ([conv.fpint]/1 — not a clamp), so a literal above u16::max must be rejected
  // here, BEFORE the cast. Integrality alone does not make the cast safe.
  constexpr atx::f64 kMaxWindow = static_cast<atx::f64>(std::numeric_limits<atx::u16>::max());
  if (floored > kMaxWindow) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "window literal too large (max 65535)");
  }
  // SAFETY: the lower rail (>= 1.0) and the upper rail (<= u16::max) above prove
  // `floored` is an integral value in [1, 65535] — provably within the u16
  // destination range — so the float→int conversion is well-defined (no UB, no
  // narrowing of a fractional part since the value is already integral).
  return atx::core::Ok(static_cast<atx::u16>(floored));
}

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
[[nodiscard]] inline atx::core::Result<TypeInfo> analyze_unary(std::span<const TypeInfo> out,
                                                               const Expr &e) {
  const TypeInfo child = out[e.a];
  if (child.is_record) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "record value must be projected with .pin before use");
  }
  if (e.opcode == OpCode::Not) {
    if (child.dtype != DType::Mask) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "logical NOT requires a mask operand");
    }
    const std::array<Shape, 1> shapes{child.shape};
    return atx::core::Ok(TypeInfo{shape_unary(shapes), DType::Mask, child.lookback});
  }
  // Neg/Abs/Sign/Log: numeric only.
  if (child.dtype != DType::F64) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "arithmetic unary op requires a numeric (f64) operand");
  }
  const std::array<Shape, 1> shapes{child.shape};
  return atx::core::Ok(TypeInfo{shape_unary(shapes), DType::F64, child.lookback});
}

// Binary node: comparisons/logicals → Mask (with operand checks); arithmetic →
// F64 (both operands numeric). Shape is the broadcast-max of the two operands.
[[nodiscard]] inline atx::core::Result<TypeInfo> analyze_binary(std::span<const TypeInfo> out,
                                                                const Expr &e) {
  const TypeInfo lhs = out[e.a];
  const TypeInfo rhs = out[e.b];
  if (lhs.is_record || rhs.is_record) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "record value must be projected with .pin before use");
  }
  const std::array<Shape, 2> shapes{lhs.shape, rhs.shape};
  const Shape shape = shape_elementwise(shapes);
  const atx::u16 lb = max_child_lookback(out, e);
  if (is_compare_or_logical(e.opcode)) {
    const bool logical = (e.opcode == OpCode::And || e.opcode == OpCode::Or);
    const DType want = logical ? DType::Mask : DType::F64;
    if (lhs.dtype != want || rhs.dtype != want) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            logical ? "logical op requires mask operands"
                                    : "comparison requires numeric (f64) operands");
    }
    return atx::core::Ok(TypeInfo{shape, DType::Mask, lb});
  }
  // Arithmetic Add/Sub/Mul/Div/Pow.
  if (lhs.dtype != DType::F64 || rhs.dtype != DType::F64) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "arithmetic op requires numeric (f64) operands");
  }
  return atx::core::Ok(TypeInfo{shape, DType::F64, lb});
}

// Select (desugared ternary): cond must be Mask; then/else must be F64; result
// shape is the broadcast-max over ALL three operands. The condition DOES widen
// the result: `(close > open) ? 1 : 0` has scalar branches but a panel mask, so
// the per-cell select yields a Panel — dropping cond would mis-size the slot.
[[nodiscard]] inline atx::core::Result<TypeInfo> analyze_select(std::span<const TypeInfo> out,
                                                                const Expr &e) {
  const TypeInfo cond = out[e.a];
  const TypeInfo then_v = out[e.b];
  const TypeInfo else_v = out[e.c];
  if (cond.is_record || then_v.is_record || else_v.is_record) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "record value must be projected with .pin before use");
  }
  if (cond.dtype != DType::Mask) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "SELECT condition must be a mask");
  }
  if (then_v.dtype != DType::F64 || else_v.dtype != DType::F64) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "SELECT branches must be numeric (f64)");
  }
  const std::array<Shape, 3> shapes{cond.shape, then_v.shape, else_v.shape};
  return atx::core::Ok(TypeInfo{shape_elementwise(shapes), DType::F64, max_child_lookback(out, e)});
}

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
[[nodiscard]] inline atx::core::Status
validate_stateful_op_dtypes(OpCode op, std::span<const TypeInfo> out, const Expr &e) {
  if (op == OpCode::TradeWhen) {
    // trade_when(trigger, alpha, exit): trigger (arg0) and exit (arg2) are masks.
    if (out[e.a].dtype != DType::Mask || out[e.c].dtype != DType::Mask) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "trade_when requires mask trigger/exit (args 1 and 3)");
    }
    if (out[e.b].dtype != DType::F64) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "trade_when alpha (arg 2) must be numeric (f64)");
    }
  }
  if (op == OpCode::Hump) {
    // hump(x, threshold): x (arg0) is numeric; optional threshold (arg1) too.
    if (out[e.a].dtype != DType::F64) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "hump requires a numeric (f64) primary operand");
    }
    if (e.b != kNoExpr && out[e.b].dtype != DType::F64) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "hump threshold (arg 2) must be numeric (f64)");
    }
  }
  return atx::core::Ok();
}

// Call node: shape from the op's table-driven rule, dtype from the registry row
// (+ group-arg validation), lookback from the temporal family. Cs*/Ts* ops
// reject a pure-scalar primary operand.
[[nodiscard]] inline atx::core::Result<TypeInfo>
analyze_call(const Ast &ast, std::span<const TypeInfo> out, const Expr &e) {
  ATX_TRY_VOID(reject_record_operands(out, e));
  // Hparam finite-constant check: each peeled hparam must be a finite literal.
  for (atx::u8 k = 0; k < e.n_hparams; ++k) {
    if (!std::isfinite(e.hparams[k])) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "hyperparameter must be a compile-time constant");
    }
  }
  const OpCode op = e.op->opcode;
  // A Cs*/Ts* op requires a non-scalar primary (nothing to rank/roll over).
  if ((is_cross_section(op) || is_time_series(op)) && out[e.a].shape == Shape::Scalar) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "expected a panel/cross-section operand, got a scalar");
  }
  // Group-aware ops: the 2nd argument must be a Group classifier.
  if (needs_group_arg(op) && out[e.b].dtype != DType::Group) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "group operator requires a classifier (Group) 2nd argument");
  }
  ATX_TRY_VOID(validate_stateful_op_dtypes(op, out, e));
  // Collect child shapes for the table-driven shape rule.
  std::array<Shape, 3> shape_buf{};
  atx::usize n = 0;
  for (const ExprId id : std::array<ExprId, 3>{e.a, e.b, e.c}) {
    if (id != kNoExpr) {
      shape_buf.at(n++) = out[id].shape;
    }
  }
  const Shape shape = e.op->shape_of(std::span<const Shape>{shape_buf.data(), n});
  atx::u16 lb = max_child_lookback(out, e);
  if (is_shift_ts(op)) {
    ATX_TRY(const atx::u16 d, window_value(ast, e));
    lb = static_cast<atx::u16>(d + lb);
  } else if (is_rolling_ts(op)) {
    ATX_TRY(const atx::u16 d, window_value(ast, e));
    lb = static_cast<atx::u16>((d - 1) + lb);
  }
  if (!e.op->pins.empty()) {
    return atx::core::Ok(TypeInfo{shape, e.op->out_dtype, lb, true, e.op->pins});
  }
  return atx::core::Ok(TypeInfo{shape, e.op->out_dtype, lb});
}

// Member node: pin projection from a record-valued operand. Resolves the named
// pin in the record's PinSig table and returns its dtype with the record's
// shape/lookback. Rejects non-record operands and unknown pin names.
[[nodiscard]] inline atx::core::Result<TypeInfo> analyze_member(std::span<const TypeInfo> out,
                                                                const Ast &ast, const Expr &e) {
  const TypeInfo rec = out[e.a];
  if (!rec.is_record) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "member access '.' requires a record-valued operand");
  }
  const std::string_view pin = ast.field_name(e.name_id);
  for (const PinSig &ps : rec.pins) {
    if (ps.name == pin) {
      return atx::core::Ok(TypeInfo{rec.shape, ps.dtype, rec.lookback, false, {}});
    }
  }
  return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                        std::string{"no pin '"} + std::string{pin} + "' on record");
}

// Dispatch one node to its kind-specific analyzer. `out` holds the already-
// computed TypeInfo of every node with a smaller ExprId (children come first).
[[nodiscard]] inline atx::core::Result<TypeInfo>
analyze_node(const Ast &ast, std::span<const TypeInfo> out, const Expr &e) {
  switch (e.kind) {
  case Expr::Kind::Literal:
    return atx::core::Ok(TypeInfo{Shape::Scalar, DType::F64, 0});
  case Expr::Kind::Field: {
    const DType dt = is_group_field(ast.field_name(e.name_id)) ? DType::Group : DType::F64;
    return atx::core::Ok(TypeInfo{Shape::Panel, dt, 0});
  }
  case Expr::Kind::Unary:
    return analyze_unary(out, e);
  case Expr::Kind::Binary:
    return analyze_binary(out, e);
  case Expr::Kind::Call:
    return analyze_call(ast, out, e);
  case Expr::Kind::Select:
    return analyze_select(out, e);
  case Expr::Kind::Member:
    return analyze_member(out, ast, e);
  }
  return atx::core::Err(atx::core::ErrorCode::Internal,
                        "analyze: unhandled Expr::Kind"); // unreachable
}

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
[[nodiscard]] inline atx::core::Result<Analysis> analyze(const Ast &ast) {
  const std::span<const Expr> arena = ast.nodes();
  Analysis result;
  result.reserve(arena.size());
  for (atx::usize i = 0; i < arena.size(); ++i) {
    // SAFETY: a node only references children with strictly smaller ids (the
    // parser appends children first), so every referenced TypeInfo is already
    // present in result.nodes() at this point.
    ATX_TRY(const TypeInfo t, detail::analyze_node(ast, result.nodes(), arena[i]));
    result.push(t);
  }

  atx::u16 required = 0;
  for (const Assignment &root : ast.roots()) {
    if (root.root != kNoExpr) {
      const TypeInfo &root_ti = result.info(root.root);
      if (root_ti.is_record) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "a record value cannot be an alpha output; project a pin with .pin");
      }
      const atx::u16 lb = root_ti.lookback;
      required = (lb > required) ? lb : required;
    }
  }
  result.set_required_lookback(required);
  return atx::core::Ok(std::move(result));
}

} // namespace atx::engine::alpha
