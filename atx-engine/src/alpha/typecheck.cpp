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
  // BRAIN-superset rolling ops (S3.2): all trailing-window, same lookback rule.
  case OpCode::TsRegression:
  case OpCode::TsDecayExp:
  case OpCode::TsEntropy:
  case OpCode::TsMoment:
  // OU rolling-fit ops (P3d-E3): windowed, same lookback rule as ts_mean.
  case OpCode::OuTheta:
  case OpCode::OuHalflife:
  case OpCode::OuMean:
  case OpCode::OuZscore:
  // W2 literature trailing-window ops: same (d-1)+child lookback rule.
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
  case OpCode::CsResidualize:
  case OpCode::CsQuantile:
  case OpCode::CsVecSum:
  case OpCode::CsVecAvg:
  case OpCode::TradeWhen:
  case OpCode::Hump:
  case OpCode::KalmanLevel:
  case OpCode::OuFilter:
  case OpCode::Pin:
  case OpCode::Split2:
  case OpCode::KalmanReg:
  case OpCode::StoreAlpha:
  case OpCode::Free:
  case OpCode::ArgPack:
  case OpCode::CsBucket:
  case OpCode::GroupCross:
  case OpCode::CsResidOn:
  case OpCode::CsSumG:
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
  case OpCode::TsDecayExp:
    // ts_decay_exp(x, d, f): decay base f must be strictly positive.
    if (e.hparams[0] <= 0.0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "ts_decay_exp: decay factor f must be > 0");
    }
    break;
  case OpCode::TsMoment:
    // ts_moment(x, d, k): central-moment order k must be an integer >= 1.
    if (e.hparams[0] < 1.0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "ts_moment: moment order k must be >= 1");
    }
    break;
  case OpCode::TsEntropy:
    // ts_entropy(x, d, b): bucket count b must be in [1, 256].
    if (e.hparams[0] < 1.0 || e.hparams[0] > 256.0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "ts_entropy: bucket count must be in [1, 256]");
    }
    break;
  default:
    break; // non-filter ops: no hparam ranges
  }
  return atx::core::Ok();
}

atx::core::Status validate_node_contract(const Expr &e) {
  const OpSig &sig = *e.op;
  // The peeled-hparam count must match the op's declared count. A swap that
  // changed the op keeps these in sync (mutation sets e.n_hparams = op n_hparams);
  // a mismatch means the node's hparam structure is inconsistent with the op.
  if (e.n_hparams != sig.n_hparams) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "op contract: hparam count does not match the operator");
  }
  // The materialized operand-slot count must be a valid arity for this op. This
  // is what rejects a finite-default op (kernel reads operand 2) swapped onto a
  // node that only materialized operand 1, or a node carrying too many operands.
  const atx::usize ar = call_arity(e);
  if (ar < operand_min_arity(sig) || ar > operand_max_arity(sig)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "op contract: materialized operand count is out of range for the operator");
  }
  return atx::core::Ok();
}

atx::core::Status validate_scalar_literal_operand(const Ast &ast, const Expr &e) {
  if (!has_scalar_literal_slot(e.op->opcode) || e.b == kNoExpr) {
    return atx::core::Ok();
  }
  const Expr &s = ast.node(e.b);
  if (s.kind != Expr::Kind::Literal || !std::isfinite(s.value)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          std::string{"scalar operand of '"} + std::string{e.op->name} +
                              "' (arg 2) must be a finite compile-time literal");
  }
  // CsQuantile truncates its bucket count to `int` (cs_quantile_row / the oracle);
  // a finite literal whose truncation is outside int's range would make that
  // static_cast UB, so the slot is bounded here. n < 2 stays legal (NaN output).
  if (e.op->opcode == OpCode::CsQuantile) {
    constexpr atx::f64 kIntLoExcl = static_cast<atx::f64>(std::numeric_limits<int>::min()) - 1.0;
    constexpr atx::f64 kIntHiExcl = static_cast<atx::f64>(std::numeric_limits<int>::max()) + 1.0;
    if (!(s.value > kIntLoExcl && s.value < kIntHiExcl)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "quantile: bucket count (arg 2) must fit in a 32-bit int");
    }
  }
  return atx::core::Ok();
}

// ===========================================================================
//  W2 literature ops (A7): the complete argument rail set of every is_lit_op.
//  Dtype refusals are strict in both directions: a Group classifier is never
//  accepted where a numeric vector is required, and a numeric vector is never
//  accepted where a Group classifier is required.
// ===========================================================================
namespace {

[[nodiscard]] atx::core::Status lit_fail(const Expr &e, std::string_view what) {
  return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                        std::string{e.op->name} + ": " + std::string{what});
}

// A numeric vector operand: F64, not a record, not a bare scalar literal.
[[nodiscard]] bool is_f64_vector(const TypeInfo &t) noexcept {
  return !t.is_record && t.dtype == DType::F64 && t.shape != Shape::Scalar;
}

[[nodiscard]] bool is_group_label(const TypeInfo &t) noexcept {
  return !t.is_record && t.dtype == DType::Group;
}

// Columns a regressor operand supplies: 1 for a numeric vector, the pin count of
// a pack2/pack3 record, 0 for anything else (Group, Mask, scalar, other record).
[[nodiscard]] atx::usize regressor_width(const Ast &ast, std::span<const TypeInfo> out,
                                         ExprId id) {
  const TypeInfo &t = out[id];
  if (is_f64_vector(t)) {
    return 1;
  }
  const Expr &c = ast.node(id);
  const bool pack = t.is_record && c.kind == Expr::Kind::Call && c.op != nullptr &&
                    c.op->opcode == OpCode::ArgPack;
  return pack ? t.pins.size() : 0;
}

// An integer-valued peeled count within [lo, hi]. The kernels truncate it to an
// integer, so a fractional or out-of-range literal is a type error, not a floor.
[[nodiscard]] bool hparam_count_in(atx::f64 v, atx::f64 lo, atx::f64 hi) noexcept {
  return std::isfinite(v) && v == std::floor(v) && v >= lo && v <= hi;
}

// (d-1) + child lookback, refusing a u16 overflow instead of wrapping.
[[nodiscard]] atx::core::Result<atx::u16> lit_ts_lookback(atx::u16 d, atx::u16 child) {
  const atx::u32 lb = static_cast<atx::u32>(d) - 1U + static_cast<atx::u32>(child);
  if (lb > std::numeric_limits<atx::u16>::max()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "lookback exceeds 65535 bars");
  }
  return atx::core::Ok(static_cast<atx::u16>(lb));
}

// Operand rails of the W2 trailing-window ops; `d` is the validated window.
[[nodiscard]] atx::core::Status lit_ts_rails(const Ast &ast, std::span<const TypeInfo> out,
                                             const Expr &e, atx::u16 d) {
  if (!is_f64_vector(out[e.a])) {
    return lit_fail(e, "primary operand must be a numeric (f64) vector");
  }
  const atx::f64 dd = static_cast<atx::f64>(d);
  switch (e.op->opcode) {
  case OpCode::TsTopkMean:
    return hparam_count_in(e.hparams[0], 1.0, dd) ? atx::core::Ok()
                                                  : lit_fail(e, "k must be an integer in [1, w]");
  case OpCode::TsCountIncreases:
    return atx::core::Ok();
  case OpCode::TsResidOn:
  case OpCode::TsBetaOn: {
    const atx::usize k = regressor_width(ast, out, e.b);
    if (k == 0 || k > kMaxTsRegressors) {
      return lit_fail(e, "regressors must be 1..3 numeric (f64) vectors");
    }
    if (static_cast<atx::usize>(d) < k + 2) {
      return lit_fail(e, "window must be >= regressor count + 2");
    }
    return atx::core::Ok();
  }
  case OpCode::TsCorrMp:
    if (!is_f64_vector(out[e.b])) {
      return lit_fail(e, "second series must be a numeric (f64) vector");
    }
    break;
  default:
    break; // the unary min-periods family
  }
  return hparam_count_in(e.hparams[0], 1.0, dd)
             ? atx::core::Ok()
             : lit_fail(e, "min periods m must be an integer in [1, w]");
}

// Operand rails of the W2 non-temporal ops (pack / bucket / group_cross /
// cs_resid_on).
[[nodiscard]] atx::core::Status lit_cs_rails(const Ast &ast, std::span<const TypeInfo> out,
                                             const Expr &e) {
  switch (e.op->opcode) {
  case OpCode::ArgPack:
    for (const ExprId id : std::array<ExprId, 3>{e.a, e.b, e.c}) {
      if (id != kNoExpr && !is_f64_vector(out[id])) {
        return lit_fail(e, "pack operands must be numeric (f64) vectors");
      }
    }
    return atx::core::Ok();
  case OpCode::CsBucket:
    if (!is_f64_vector(out[e.a])) {
      return lit_fail(e, "primary operand must be a numeric (f64) vector");
    }
    return hparam_count_in(e.hparams[0], 2.0, 65535.0)
               ? atx::core::Ok()
               : lit_fail(e, "bucket count n must be an integer in [2, 65535]");
  case OpCode::GroupCross:
    return (is_group_label(out[e.a]) && is_group_label(out[e.b]))
               ? atx::core::Ok()
               : lit_fail(e, "both operands must be Group classifiers");
  case OpCode::CsResidOn: {
    if (!is_f64_vector(out[e.a])) {
      return lit_fail(e, "primary operand must be a numeric (f64) vector");
    }
    const atx::usize kb = regressor_width(ast, out, e.b);
    const atx::usize kc = (e.c == kNoExpr) ? 0 : regressor_width(ast, out, e.c);
    if (kb == 0 || (e.c != kNoExpr && kc == 0) || kb + kc > kMaxCsCovariates) {
      return lit_fail(e, "covariates must be 1..4 numeric (f64) vectors");
    }
    return atx::core::Ok();
  }
  default:
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "analyze_lit_call: not a W2 non-temporal op");
  }
}

} // namespace

atx::core::Result<TypeInfo> analyze_lit_call(const Ast &ast, std::span<const TypeInfo> out,
                                             const Expr &e) {
  ATX_TRY_VOID(validate_node_contract(e));
  for (atx::u8 k = 0; k < e.n_hparams; ++k) {
    if (!std::isfinite(e.hparams[k])) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "hyperparameter must be a compile-time constant");
    }
  }
  std::array<Shape, 3> shape_buf{};
  atx::usize n = 0;
  for (const ExprId id : std::array<ExprId, 3>{e.a, e.b, e.c}) {
    if (id != kNoExpr) {
      shape_buf.at(n++) = out[id].shape;
    }
  }
  const Shape shape = e.op->shape_of(std::span<const Shape>{shape_buf.data(), n});
  const atx::u16 child_lb = max_child_lookback(out, e);
  const OpCode op = e.op->opcode;
  if (is_lit_ts_op(op)) {
    ATX_TRY(const atx::u16 d, window_value(ast, e));
    ATX_TRY_VOID(lit_ts_rails(ast, out, e, d));
    ATX_TRY(const atx::u16 lb, lit_ts_lookback(d, child_lb));
    return atx::core::Ok(TypeInfo{shape, e.op->out_dtype, lb});
  }
  ATX_TRY_VOID(lit_cs_rails(ast, out, e));
  if (op == OpCode::ArgPack) {
    return atx::core::Ok(TypeInfo{shape, DType::F64, child_lb, true, e.op->pins});
  }
  return atx::core::Ok(TypeInfo{shape, e.op->out_dtype, child_lb});
}

atx::core::Result<TypeInfo> analyze_call(const Ast &ast, std::span<const TypeInfo> out,
                                         const Expr &e) {
  if (is_lit_op(e.op->opcode)) {
    return analyze_lit_call(ast, out, e); // W2 ops own their complete rail set
  }
  ATX_TRY_VOID(reject_record_operands(out, e));
  ATX_TRY_VOID(validate_node_contract(e));
  ATX_TRY_VOID(validate_scalar_literal_operand(ast, e)); // A-03
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
  // Task 3.2: belt-and-suspenders input-dtype guard. Every Cs*/Ts*/filter op
  // consumes e.a as a NUMERIC (F64) signal; a Group classifier in e.a is always
  // a bug (the classifier role is e.b). Reject hard so zscore(sector),
  // ts_mean(sector,5), etc. are Err regardless of how the genome was produced.
  if (needs_panel_primary && out[e.a].dtype != DType::F64) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "operator requires a numeric (f64) primary operand; "
                          "got a Group classifier");
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
  // cs_residualize's optional style covariate (3rd arg, when present) must be a
  // non-scalar numeric (F64) panel/cross-section column — a continuous regressor.
  if (op == OpCode::CsResidualize && e.c != kNoExpr) {
    if (out[e.c].dtype != DType::F64) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "cs_residualize style covariate (arg 3) must be numeric (f64)");
    }
    if (out[e.c].shape == Shape::Scalar) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "cs_residualize style covariate (arg 3) must be a panel/cross-section");
    }
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
