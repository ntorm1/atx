#include "atx/engine/alpha/typecheck.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string>

namespace atx::engine::alpha {

namespace detail {

bool is_rolling_ts(OpCode op) noexcept {
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
  // OU rolling-fit ops (P3d-E3): windowed, same lookback rule as ts_mean.
  case OpCode::OuTheta:
  case OpCode::OuHalflife:
  case OpCode::OuMean:
  case OpCode::OuZscore:
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
  case OpCode::KalmanLevel:
  case OpCode::OuFilter:
  case OpCode::Pin:
  case OpCode::Split2:
  case OpCode::KalmanReg:
  case OpCode::StoreAlpha:
  case OpCode::Free:
    return false;
  }
  return false; // unreachable for valid OpCode
}

atx::core::Result<atx::u16> window_value(const Ast &ast, const Expr &call) {
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

atx::core::Result<TypeInfo> analyze_unary(std::span<const TypeInfo> out, const Expr &e) {
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

atx::core::Result<TypeInfo> analyze_binary(std::span<const TypeInfo> out, const Expr &e) {
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

atx::core::Result<TypeInfo> analyze_select(std::span<const TypeInfo> out, const Expr &e) {
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

atx::core::Status validate_stateful_op_dtypes(OpCode op, std::span<const TypeInfo> out,
                                              const Expr &e) {
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

atx::core::Status validate_hparam_ranges(OpCode op, const Expr &e) {
  switch (op) {
  case OpCode::KalmanLevel:
    // Q (process noise) >= 0; R (observation noise) > 0.
    if (e.hparams[0] < 0.0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "kalman_level: Q (process noise) must be >= 0");
    }
    if (e.hparams[1] <= 0.0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "kalman_level: R (observation noise) must be > 0");
    }
    break;
  case OpCode::OuFilter:
    // theta (mean-reversion rate) >= 0; mu (long-run mean) is only required
    // finite (already guaranteed by analyze_call's isfinite loop).
    if (e.hparams[0] < 0.0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "ou_filter: theta (mean-reversion rate) must be >= 0");
    }
    break;
  case OpCode::KalmanReg:
    // delta in (0,1) strict: sets process noise W = (delta/(1-delta))*I2.
    if (e.hparams[0] <= 0.0 || e.hparams[0] >= 1.0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "kalman: delta must be in (0, 1) exclusive");
    }
    // R (observation noise) must be strictly positive.
    if (e.hparams[1] <= 0.0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "kalman: R (observation noise) must be > 0");
    }
    break;
  default:
    break; // non-filter ops: no hparam ranges
  }
  return atx::core::Ok();
}

atx::core::Result<TypeInfo> analyze_call(const Ast &ast, std::span<const TypeInfo> out,
                                         const Expr &e) {
  ATX_TRY_VOID(reject_record_operands(out, e));
  // Hparam finite-constant check: each peeled hparam must be a finite literal.
  for (atx::u8 k = 0; k < e.n_hparams; ++k) {
    if (!std::isfinite(e.hparams[k])) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "hyperparameter must be a compile-time constant");
    }
  }
  const OpCode op = e.op->opcode;
  // A Cs*/Ts* op or filter recurrence op requires a non-scalar primary.
  const bool needs_panel_primary = is_cross_section(op) || is_time_series(op) ||
                                   op == OpCode::KalmanLevel || op == OpCode::OuFilter ||
                                   op == OpCode::KalmanReg;
  if (needs_panel_primary && out[e.a].shape == Shape::Scalar) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "expected a panel/cross-section operand, got a scalar");
  }
  // KalmanReg requires BOTH y (arg0) and x (arg1) to be non-scalar Panel operands.
  if (op == OpCode::KalmanReg && e.b != kNoExpr && out[e.b].shape == Shape::Scalar) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "kalman: both y and x operands must be panel signals");
  }
  // Group-aware ops: the 2nd argument must be a Group classifier.
  if (needs_group_arg(op) && out[e.b].dtype != DType::Group) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "group operator requires a classifier (Group) 2nd argument");
  }
  ATX_TRY_VOID(validate_stateful_op_dtypes(op, out, e));
  // Filter hparam range checks (hparams already verified finite by the loop above).
  ATX_TRY_VOID(validate_hparam_ranges(op, e));
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

atx::core::Result<TypeInfo> analyze_member(std::span<const TypeInfo> out, const Ast &ast,
                                           const Expr &e) {
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

atx::core::Result<TypeInfo> analyze_node(const Ast &ast, std::span<const TypeInfo> out,
                                         const Expr &e) {
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

atx::core::Result<Analysis> analyze(const Ast &ast) {
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
