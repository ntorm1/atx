#include "atx/engine/alpha/registry.hpp"

#include <limits>
#include <string>

namespace atx::engine::alpha {

namespace detail {

// The complete built-in catalogue (Appendix A named functions). Kept as a
// static span so construction is a single copy. `consteval`-friendly literals;
// the array has static storage, so every `name` view is non-dangling forever.
[[nodiscard]] std::span<const OpSig> builtin_ops() noexcept {
  // Rows are positional: {name, min_arity, max_arity, opcode, out_dtype,
  // lookahead_safe, defaults, shape_of}. Fixed-arity ops carry min==max and an
  // empty defaults array. `scale` is the lone variadic built-in in 3b: 1
  // required arg, 1 optional with a finite default of 1.0 (P3b-1).
  // P3d-B3 adds two trailing OpSig fields (n_hparams, pins) with member-
  // initializers — existing rows omit them and pick up {0, {}} automatically.
  static constexpr std::array<OpSig, 70> kOps = {{
      // ---- unary element-wise functions (P→P) ----
      {"abs", 1, 1, OpCode::Abs, DType::F64, true, {}, &shape_unary},
      {"sign", 1, 1, OpCode::Sign, DType::F64, true, {}, &shape_unary},
      {"log", 1, 1, OpCode::Log, DType::F64, true, {}, &shape_unary},
      // BRAIN-superset element-wise activations (P3b-2): NaN→NaN naturally.
      {"sigmoid", 1, 1, OpCode::Sigmoid, DType::F64, true, {}, &shape_unary},
      {"tanh", 1, 1, OpCode::Tanh, DType::F64, true, {}, &shape_unary},
      // ---- binary element-wise functions ----
      {"power", 2, 2, OpCode::Pow, DType::F64, true, {}, &shape_elementwise},
      {"signedpower", 2, 2, OpCode::Spow, DType::F64, true, {}, &shape_elementwise},
      {"min", 2, 2, OpCode::MinP, DType::F64, true, {}, &shape_elementwise},
      {"max", 2, 2, OpCode::MaxP, DType::F64, true, {}, &shape_elementwise},
      // ---- cross-sectional (P→V) ----
      {"rank", 1, 1, OpCode::CsRank, DType::F64, true, {}, &shape_cross_section},
      {"zscore", 1, 1, OpCode::CsZscore, DType::F64, true, {}, &shape_cross_section},
      // scale(x) defaults its 2nd arg to 1.0 (target L1 norm).
      {"scale", 1, 2, OpCode::CsScale, DType::F64, true, {1.0}, &shape_cross_section},
      // BRAIN-superset cross-sectional (P3b-2). normalize = cross-sectional
      // demean; winsorize(x, std=4) clamps to mean±std·σ (σ sample, ddof=1),
      // reading the `std` multiplier from its 2nd operand like CsScale's factor.
      {"normalize", 1, 1, OpCode::CsNormalize, DType::F64, true, {}, &shape_cross_section},
      {"winsorize", 1, 2, OpCode::CsWinsorize, DType::F64, true, {4.0}, &shape_cross_section},
      {"indneutralize", 2, 2, OpCode::CsDemeanG, DType::F64, true, {}, &shape_cross_section},
      // group_neutralize stays fixed-arity 2 in P3b-1; optional cap → P3b-4.
      {"group_neutralize", 2, 2, OpCode::CsNeutG, DType::F64, true, {}, &shape_cross_section},
      {"group_rank", 2, 2, OpCode::CsRankG, DType::F64, true, {}, &shape_cross_section},
      {"group_zscore", 2, 2, OpCode::CsZscoreG, DType::F64, true, {}, &shape_cross_section},
      // BRAIN-superset group aggregates (P3b-2): arg2 = Group classifier. Each
      // broadcasts a within-group aggregate over its members (NaN-fill excluded).
      {"group_count", 2, 2, OpCode::CsCountG, DType::F64, true, {}, &shape_cross_section},
      {"group_mean", 2, 2, OpCode::CsMeanG, DType::F64, true, {}, &shape_cross_section},
      {"group_scale", 2, 2, OpCode::CsScaleG, DType::F64, true, {}, &shape_cross_section},
      // Regression-residual neutralization (S3.1). cs_residualize(x, g) is the
      // per-group demean (the boundary pin == indneutralize); cs_residualize(x,
      // g, z) adds a continuous style covariate (FWL partial-out). The optional
      // 3rd arg carries a NaN-sentinel default, so an omitted z is left ABSENT
      // (arg c == kNoExpr) rather than materialized — the kernel handles both.
      {"cs_residualize", 2, 3, OpCode::CsResidualize, DType::F64, true,
       {std::numeric_limits<atx::f64>::quiet_NaN()}, &shape_cross_section},
      // ---- time-series (P→P) ----
      {"delay", 2, 2, OpCode::TsDelay, DType::F64, true, {}, &shape_panel},
      {"delta", 2, 2, OpCode::TsDelta, DType::F64, true, {}, &shape_panel},
      {"ts_sum", 2, 2, OpCode::TsSum, DType::F64, true, {}, &shape_panel},
      {"ts_mean", 2, 2, OpCode::TsMean, DType::F64, true, {}, &shape_panel},
      {"stddev", 2, 2, OpCode::TsStd, DType::F64, true, {}, &shape_panel},
      {"ts_std", 2, 2, OpCode::TsStd, DType::F64, true, {}, &shape_panel},
      {"ts_var", 2, 2, OpCode::TsVar, DType::F64, true, {}, &shape_panel},
      {"ts_min", 2, 2, OpCode::TsMin, DType::F64, true, {}, &shape_panel},
      {"ts_max", 2, 2, OpCode::TsMax, DType::F64, true, {}, &shape_panel},
      {"ts_argmin", 2, 2, OpCode::TsArgMin, DType::F64, true, {}, &shape_panel},
      {"ts_argmax", 2, 2, OpCode::TsArgMax, DType::F64, true, {}, &shape_panel},
      {"ts_rank", 2, 2, OpCode::TsRank, DType::F64, true, {}, &shape_panel},
      {"correlation", 3, 3, OpCode::TsCorr, DType::F64, true, {}, &shape_panel},
      {"covariance", 3, 3, OpCode::TsCov, DType::F64, true, {}, &shape_panel},
      {"product", 2, 2, OpCode::TsProduct, DType::F64, true, {}, &shape_panel},
      {"decay_linear", 2, 2, OpCode::TsDecayLinear, DType::F64, true, {}, &shape_panel},
      {"ema", 2, 2, OpCode::TsEma, DType::F64, true, {}, &shape_panel},
      {"wma", 2, 2, OpCode::TsWma, DType::F64, true, {}, &shape_panel},
      {"skew", 2, 2, OpCode::TsSkew, DType::F64, true, {}, &shape_panel},
      {"kurt", 2, 2, OpCode::TsKurt, DType::F64, true, {}, &shape_panel},
      {"med", 2, 2, OpCode::TsMed, DType::F64, true, {}, &shape_panel},
      {"mad", 2, 2, OpCode::TsMad, DType::F64, true, {}, &shape_panel},
      {"slope", 2, 2, OpCode::TsSlope, DType::F64, true, {}, &shape_panel},
      {"rsquare", 2, 2, OpCode::TsRsquare, DType::F64, true, {}, &shape_panel},
      {"resid", 2, 2, OpCode::TsResid, DType::F64, true, {}, &shape_panel},
      // BRAIN-superset rolling (P3b-2). All full-window min_periods except
      // ts_backfill (looks PAST NaNs to the most recent valid value in [t-d+1,t]).
      {"ts_zscore", 2, 2, OpCode::TsZscore, DType::F64, true, {}, &shape_panel},
      {"ts_backfill", 2, 2, OpCode::TsBackfill, DType::F64, true, {}, &shape_panel},
      {"ts_av_diff", 2, 2, OpCode::TsAvDiff, DType::F64, true, {}, &shape_panel},
      {"ts_quantile", 2, 2, OpCode::TsQuantile, DType::F64, true, {}, &shape_panel},
      {"ts_scale", 2, 2, OpCode::TsScale, DType::F64, true, {}, &shape_panel},
      {"ts_count_nans", 2, 2, OpCode::TsCountNans, DType::F64, true, {}, &shape_panel},
      {"ts_skew", 2, 2, OpCode::TsSkew, DType::F64, true, {}, &shape_panel},
      {"ts_kurt", 2, 2, OpCode::TsKurt, DType::F64, true, {}, &shape_panel},
      {"ts_corr", 3, 3, OpCode::TsCorr, DType::F64, true, {}, &shape_panel},
      // ---- BRAIN-superset rolling ops (S3.2) --------------------------------
      // ts_regression(y, x, d): rolling OLS slope of y on x — binary-series like
      // correlation/covariance (n_hparams=0; window is the 3rd operand).
      {"ts_regression", 3, 3, OpCode::TsRegression, DType::F64, true, {}, &shape_panel},
      // ts_decay_exp(x, d, f): exponential decay, weight f^k (newest heaviest);
      // ts_moment(x, d, k): k-th central moment; ts_entropy(x, d, b): rolling
      // Shannon entropy over b buckets. The trailing arg (f/k/b) is peeled as a
      // compile-time hparam (n_hparams=1) into imm[0]; operands are (x, window).
      {"ts_decay_exp", 3, 3, OpCode::TsDecayExp, DType::F64, true, {}, &shape_panel, 1, {}},
      {"ts_moment", 3, 3, OpCode::TsMoment, DType::F64, true, {}, &shape_panel, 1, {}},
      {"ts_entropy", 3, 3, OpCode::TsEntropy, DType::F64, true, {}, &shape_panel, 1, {}},
      // ---- stateful recurrence (P3b-3): output Panel; both are CAUSAL (the
      //      forward scan seeds at the panel's first date and reads only the
      //      prior state + inputs <= t) so lookahead_safe = true. shape_panel
      //      (always Panel) is correct: the per-instrument recurrence yields a
      //      date x instrument block, mirroring the Ts* P->P shape rule.
      //   trade_when(trigger, alpha, exit) — arity 3; trigger/exit are masks,
      //   alpha is F64; the typechecker pins the per-arg dtypes (analyze_call).
      {"trade_when", 3, 3, OpCode::TradeWhen, DType::F64, true, {}, &shape_panel},
      //   hump(x, threshold=0.01) — arity (1,2); the optional threshold uses the
      //   P3b-1 default machinery (default-fill materializes Literal 0.01).
      {"hump", 1, 2, OpCode::Hump, DType::F64, true, {0.01}, &shape_panel},
      //   kalman_level(x, Q, R) — scalar local-level Kalman filter; Q>=0, R>0.
      //   Q and R are peeled as hparams (n_hparams=2); x is the panel primary.
      {"kalman_level", 3, 3, OpCode::KalmanLevel, DType::F64, true, {}, &shape_panel, 2, {}},
      //   ou_filter(x, theta, mu) — OU AR(1) pull-to-mean smoother; theta>=0.
      //   theta and mu are peeled as hparams (n_hparams=2); x is the panel primary.
      {"ou_filter", 3, 3, OpCode::OuFilter, DType::F64, true, {}, &shape_panel, 2, {}},
      // ---- multi-output test builtin (P3d-B3) --------------------------------
      // split2(x) — synthetic 2-pin record op used to validate the multi-output
      // IR before real filter kernels land in B9. Registers Pin/Split2 opcodes
      // and exercises the PinSig/pins machinery. NOT emitted by any program yet.
      {"split2",
       1,
       1,
       OpCode::Split2,
       DType::F64,
       true,
       {},
       &shape_panel,
       0,
       std::span<const PinSig>{kSplit2Pins}},
      // ---- Chan 2-state time-varying regression record op (P3d-D2) -----------
      // kalman(y, x, delta, R) — arity 4; y and x are panel operands (2 non-
      // hparam args); delta and R are compile-time hparams (n_hparams=2).
      // delta in (0,1) strict; R > 0 (typecheck enforces). Outputs 3 pins:
      // alpha (intercept), beta (slope), resid (standardised innovation).
      {"kalman",
       4,
       4,
       OpCode::KalmanReg,
       DType::F64,
       true,
       {},
       &shape_panel,
       2,
       std::span<const PinSig>{kKalmanRegPins}},
      // ---- OU rolling-fit ops (P3d-E3): windowed time-series, arity 2 -----
      // Each fits AR(1) OLS over the trailing window and derives an OU quantity.
      // Window is the 2nd operand (a literal constant). n_hparams=0, no pins.
      {"ou_theta",    2, 2, OpCode::OuTheta,    DType::F64, true, {}, &shape_panel},
      {"ou_halflife", 2, 2, OpCode::OuHalflife, DType::F64, true, {}, &shape_panel},
      {"ou_mean",     2, 2, OpCode::OuMean,     DType::F64, true, {}, &shape_panel},
      {"ou_zscore",   2, 2, OpCode::OuZscore,   DType::F64, true, {}, &shape_panel},
  }};
  return kOps;
}

} // namespace detail

Library::Library() {
  const std::span<const OpSig> builtins = detail::builtin_ops();
  ops_.assign(builtins.begin(), builtins.end());
}

atx::core::Status Library::register_op(const OpSig &sig) {
  if (sig.name.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "register_op: operator name must not be empty");
  }
  if (sig.shape_of == nullptr) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          std::string{"register_op: shape_of must not be null for '"} +
                              std::string{sig.name} + "'");
  }
  if (find(sig.name) != nullptr) {
    return atx::core::Err(atx::core::ErrorCode::AlreadyExists,
                          std::string{"register_op: duplicate operator '"} + std::string{sig.name} +
                              "'");
  }
  // Arity-range well-formedness. The parser's default-fill indexes
  // `defaults[k - min_arity]` for k in [min_arity, max_arity), so the optional
  // count (max_arity - min_arity) MUST fit OpSig::defaults; an inverted range
  // is meaningless. Both checks keep fill_default_args in-bounds (no UB).
  if (sig.max_arity < sig.min_arity) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          std::string{"register_op: max_arity < min_arity for '"} +
                              std::string{sig.name} + "'");
  }
  if (sig.max_arity - sig.min_arity > kMaxDefaults) {
    return atx::core::Err(
        atx::core::ErrorCode::InvalidArgument,
        std::string{"register_op: optional-arg count exceeds kMaxDefaults for '"} +
            std::string{sig.name} + "'");
  }
  if (sig.n_hparams > sig.max_arity) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          std::string{"register_op: n_hparams exceeds arity for '"} +
                              std::string{sig.name} + "'");
  }
  if (!sig.pins.empty() && sig.pins.size() < 2) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          std::string{"register_op: record op needs >=2 pins for '"} +
                              std::string{sig.name} + "'");
  }
  ops_.push_back(sig);
  return atx::core::Ok();
}

} // namespace atx::engine::alpha
