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
  static constexpr std::array<OpSig, 74> kOps = {{
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
      // Cross-sectional gap-fill ops (S3.3). quantile(x[, n]) discretizes the
      // valid set into n buckets (default 5; n read from the scalar 2nd operand
      // exactly like winsorize's std/CsScale's factor); reverse(x) = -x (routes
      // to Neg — the rank-reversal idiom, no new opcode); vec_sum/vec_avg reduce
      // over the valid set and broadcast the scalar back to every valid cell.
      {"quantile", 1, 2, OpCode::CsQuantile, DType::F64, true, {5.0}, &shape_cross_section},
      {"reverse", 1, 1, OpCode::Neg, DType::F64, true, {}, &shape_unary},
      {"vec_sum", 1, 1, OpCode::CsVecSum, DType::F64, true, {}, &shape_cross_section},
      {"vec_avg", 1, 1, OpCode::CsVecAvg, DType::F64, true, {}, &shape_cross_section},
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

// Platform-v7 W2 literature ops (A7). Same positional row layout as builtin_ops.
// NaN semantics per op are documented in lit_ops.hpp; the typechecker
// (analyze_lit_call) owns every argument rail listed here.
[[nodiscard]] std::span<const OpSig> literature_ops() noexcept {
  constexpr atx::f64 kAbsent = std::numeric_limits<atx::f64>::quiet_NaN();
  static constexpr std::array<OpSig, 17> kLit = {{
      // pack2(a, b) / pack3(a, b, c): regressor bundle (record, one block).
      {"pack2", 2, 2, OpCode::ArgPack, DType::F64, true, {}, &shape_elementwise, 0,
       std::span<const PinSig>{kPack2Pins}},
      {"pack3", 3, 3, OpCode::ArgPack, DType::F64, true, {}, &shape_elementwise, 0,
       std::span<const PinSig>{kPack3Pins}},
      // ts_topk_mean(x, w, k): k peeled into imm[0] (like ts_moment's k).
      {"ts_topk_mean", 3, 3, OpCode::TsTopkMean, DType::F64, true, {}, &shape_panel, 1, {}},
      // bucket(x, n) -> Group: n peeled into imm[0]; a classifier, not a signal.
      {"bucket", 2, 2, OpCode::CsBucket, DType::Group, true, {}, &shape_cross_section, 1, {}},
      // group_cross(g1, g2) -> Group: element-wise product label.
      {"group_cross", 2, 2, OpCode::GroupCross, DType::Group, true, {}, &shape_elementwise},
      // ts_resid_on / ts_beta_on(y, x1[, x2[, x3]], w): the parser packs x1..xk
      // (k >= 2) into operand b; the window stays the last operand (c).
      {"ts_resid_on", 3, 5, OpCode::TsResidOn, DType::F64, true, {kAbsent, kAbsent},
       &shape_panel},
      {"ts_beta_on", 3, 5, OpCode::TsBetaOn, DType::F64, true, {kAbsent, kAbsent}, &shape_panel},
      // cs_resid_on(x, c1[, c2[, c3[, c4]]]): covariates in operands b and c.
      {"cs_resid_on", 2, 5, OpCode::CsResidOn, DType::F64, true, {kAbsent, kAbsent, kAbsent},
       &shape_cross_section},
      {"ts_count_increases", 2, 2, OpCode::TsCountIncreases, DType::F64, true, {}, &shape_panel},
      // Min-periods family: (x, w, m) with m peeled into imm[0].
      {"ts_sum_mp", 3, 3, OpCode::TsSumMp, DType::F64, true, {}, &shape_panel, 1, {}},
      {"ts_mean_mp", 3, 3, OpCode::TsMeanMp, DType::F64, true, {}, &shape_panel, 1, {}},
      {"ts_std_mp", 3, 3, OpCode::TsStdMp, DType::F64, true, {}, &shape_panel, 1, {}},
      {"ts_zscore_mp", 3, 3, OpCode::TsZscoreMp, DType::F64, true, {}, &shape_panel, 1, {}},
      {"ts_min_mp", 3, 3, OpCode::TsMinMp, DType::F64, true, {}, &shape_panel, 1, {}},
      {"ts_max_mp", 3, 3, OpCode::TsMaxMp, DType::F64, true, {}, &shape_panel, 1, {}},
      {"decay_linear_mp", 3, 3, OpCode::TsDecayLinearMp, DType::F64, true, {}, &shape_panel, 1,
       {}},
      {"ts_corr_mp", 4, 4, OpCode::TsCorrMp, DType::F64, true, {}, &shape_panel, 1, {}},
  }};
  return kLit;
}

// Platform-v8 lane YOPS formulaic ops. Same positional row layout as builtin_ops.
// Semantics: cs_ops.hpp (group_sum), asof_ops.hpp (group_delay, the as-of rank
// family); the typechecker (analyze_formulaic_call) owns the group_delay and
// as-of argument rails.
[[nodiscard]] std::span<const OpSig> formulaic_ops() noexcept {
  static constexpr std::array<OpSig, 7> kFormulaic = {{
      // group_sum(x, g): Σ x over the group's valid members (x non-NaN, the same
      // non-NaN label), broadcast to each valid member; arg 2 is a Group classifier
      // exactly as group_mean's (typecheck needs_group_arg).
      {"group_sum", 2, 2, OpCode::CsSumG, DType::F64, true, {}, &shape_cross_section},
      // group_delay(g, d) -> Group: the label d sessions ago (delay's kernel on a
      // classifier, which delay itself refuses); d is the literal 2nd operand.
      {"group_delay", 2, 2, OpCode::GroupDelay, DType::Group, true, {}, &shape_panel},
      // asof_rank_<outer>(x, w, [y,] d, j): window d and lag j peeled into imm[0],
      // imm[1]; x, w (and y) are numeric vectors.
      {"asof_rank_ts_rank", 4, 4, OpCode::AsofRankTsRank, DType::F64, true, {}, &shape_panel,
       2, {}},
      {"asof_rank_ts_min", 4, 4, OpCode::AsofRankTsMin, DType::F64, true, {}, &shape_panel, 2,
       {}},
      {"asof_rank_decay_linear", 4, 4, OpCode::AsofRankDecayLinear, DType::F64, true, {},
       &shape_panel, 2, {}},
      {"asof_rank_correlation", 5, 5, OpCode::AsofRankCorr, DType::F64, true, {}, &shape_panel,
       2, {}},
      {"asof_rank_covariance", 5, 5, OpCode::AsofRankCov, DType::F64, true, {}, &shape_panel,
       2, {}},
  }};
  return kFormulaic;
}

} // namespace detail

Library::Library() {
  const std::span<const OpSig> builtins = detail::builtin_ops();
  const std::span<const OpSig> lit = detail::literature_ops();
  const std::span<const OpSig> formulaic = detail::formulaic_ops();
  ops_.reserve(builtins.size() + lit.size() + formulaic.size());
  ops_.assign(builtins.begin(), builtins.end());
  ops_.insert(ops_.end(), lit.begin(), lit.end());
  ops_.insert(ops_.end(), formulaic.begin(), formulaic.end());
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
