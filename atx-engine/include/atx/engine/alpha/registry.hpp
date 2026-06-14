#pragma once

// atx::engine::alpha — operator registry (P3-2): the shared source of truth.
//
// This header defines the DSL's *semantic vocabulary*:
//   * OpCode — the full VM instruction set (the ISA). Every built-in operator
//     and every leaf/store/free instruction names one OpCode. P3-6/7/8 give
//     each opcode a kernel; here we only NAME them.
//   * Shape / DType — the type lattice the checker (P3-3) enforces.
//   * OpSig — one registry row per *named* operator/function: name, arity,
//     opcode, output dtype, lookahead rail, and a table-driven shape signature.
//   * Library — the name→OpSig catalogue. Built-ins are registered at
//     construction; callers may add user ops via register_op.
//
// This is the single contract the parser (P3-2), type-checker (P3-3),
// hash-consed DAG (P3-4), and VM (P3-6/7/8) all consult. Getting the enums and
// the OpSig shape right matters more than any one consumer.
//
// Header-only. All free functions are `inline` so a second translation unit
// that includes this header does not trip a multiple-definition error (the
// lexer's original missing-`inline` bug — not repeated here).
//
// Ownership / lifetime:
//   * `OpSig::name` is a view into a string literal with static storage for
//     built-ins; for user ops the caller must keep the backing string alive at
//     least as long as the Library. (Documented; not owned here.)
//   * `OpSig::shape_of` is a non-owning, non-null function pointer to a pure,
//     stateless shape rule.
//   * `Library::find` returns a non-owning `const OpSig*` borrowed from the
//     Library; it is valid until the Library is destroyed or mutated.

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

// Forward declarations (OpCode/Shape/DType are forward-declared here).
#include "atx/engine/alpha/fwd.hpp"

namespace atx::engine::alpha {

// =========================================================================
//  Type lattice (forward-declared in fwd.hpp) — underlying type MUST be u8.
// =========================================================================

// Signal shape: a literal/constant (Scalar), one value per instrument at a
// single date (CrossSection, "V"), or a date × instrument block (Panel).
enum class Shape : atx::u8 {
  Scalar,
  CrossSection,
  Panel,
};

// Element data type: numeric f64, boolean Mask (from comparisons/logical), or
// integer Group label (sector/industry classifier).
enum class DType : atx::u8 {
  F64,
  Mask,
  Group,
};

// =========================================================================
//  OpCode — the full VM instruction set (forward-declared in fwd.hpp).
//
//  Defined in full HERE (rather than the future bytecode.hpp) because the
//  registry must name each built-in's opcode now; P3-4/6/7/8 reference these
//  same enumerators. Underlying type MUST be u8 (matches fwd.hpp). Exhaustive
//  switches over this enum carry no `default`.
// =========================================================================

enum class OpCode : atx::u8 {
  // ---- leaves / housekeeping ----
  LoadField,
  Const,
  // ---- element-wise arithmetic ----
  Add,
  Sub,
  Mul,
  Div,
  Neg,
  Abs,
  Sign,
  Log,
  Sigmoid,
  Tanh,
  Pow,
  Spow,
  MinP,
  MaxP,
  // ---- comparison / logical (produce Mask) ----
  CmpLt,
  CmpGt,
  CmpLe,
  CmpGe,
  CmpEq,
  CmpNe,
  And,
  Or,
  Not,
  Select,
  // ---- cross-sectional ----
  CsRank,
  CsZscore,
  CsScale,
  CsNormalize,
  CsWinsorize,
  CsDemeanG,
  CsNeutG,
  CsRankG,
  CsZscoreG,
  CsCountG,
  CsMeanG,
  CsScaleG,
  // Regression-residual neutralization (S3.1): regress the cross-section on a
  // group-dummy design (+ an optional continuous style covariate) and emit the
  // residual. Demean (CsDemeanG) is the special case with no style covariate.
  CsResidualize,
  // ---- time-series ----
  TsDelay,
  TsDelta,
  TsSum,
  TsMean,
  TsStd,
  TsVar,
  TsMin,
  TsMax,
  TsArgMin,
  TsArgMax,
  TsRank,
  TsCorr,
  TsCov,
  TsProduct,
  TsDecayLinear,
  TsEma,
  TsWma,
  TsSkew,
  TsKurt,
  TsMed,
  TsMad,
  TsSlope,
  TsRsquare,
  TsResid,
  TsZscore,
  TsBackfill,
  TsAvDiff,
  TsQuantile,
  TsScale,
  TsCountNans,
  // BRAIN-superset rolling ops (S3.2): rolling OLS slope of y on x; exponential
  // decay (f^k weights, peeled hparam f); rolling Shannon entropy (peeled hparam
  // = bucket count); k-th central moment (peeled hparam k). Each registers in
  // is_rolling_ts; the three hparam ops bake their immediate into imm[0].
  TsRegression,
  TsDecayExp,
  TsEntropy,
  TsMoment,
  // ---- stateful recurrence (P3b-3): carry TRUE cross-date state from the
  //      panel's first date forward (no trailing window). Causal by
  //      construction — the forward scan reads only state[t-1] + inputs <= t.
  TradeWhen,
  Hump,
  // ---- stateful filter ops (P3d-C4): per-instrument causal recurrences with
  //      compile-time hyperparameters baked into the instruction as immediates.
  //      KalmanLevel: scalar local-level Kalman filter (Q, R).
  //      OuFilter: Ornstein-Uhlenbeck AR(1) pull-to-mean smoother (theta, mu).
  KalmanLevel,
  OuFilter,
  // ---- multi-output / record ops (P3d-B3): split a panel into named pins.
  //      Kernels land in B9; these enumerators exist now so the ISA is
  //      forward-declared and the registry + switch sites stay green.
  Pin,
  Split2,
  // ---- Chan 2-state time-varying regression record op (P3d-D2):
  //      kalman(y, x, delta, R) -> record {alpha, beta, resid}.
  //      3 output pins (intercept, slope, standardised residual); delta in
  //      (0,1) strict; R > 0; both y and x must be non-scalar (Panel).
  KalmanReg,
  // ---- OU rolling-fit ops (P3d-E3): windowed time-series ops (arity 2,
  //      window = operand literal, output Panel, lookback (d-1)+child).
  //      Each fits AR(1) OLS over the trailing window [t-d+1, t] per cell:
  //        OuTheta    = -ln(b)            (mean-reversion speed)
  //        OuHalflife = ln(2) / theta     (half-life of mean reversion)
  //        OuMean     = a / (1-b)         (long-run equilibrium mean)
  //        OuZscore   = (x[t]-mu)/sigma_eq (standardised deviation from mean)
  //      All yield NaN when b not in (0,1) or the fit is degenerate.
  OuTheta,
  OuHalflife,
  OuMean,
  OuZscore,
  // ---- store / free ----
  StoreAlpha,
  Free,
};

// =========================================================================
//  Shape signatures (plan §4 broadcast rules).
//
//  Each is a pure, stateless free function matching the OpSig::shape_of
//  signature. The CHECK (rejecting mismatches) is P3-3's job; here we encode
//  the *signature table* the checker will call. Args is the ordered span of
//  child shapes; `shape_of` returns this op's output shape.
// =========================================================================

namespace detail {

// Broadcast (max-shape) rule for element-wise ops: any Panel arg ⇒ Panel,
// else any CrossSection arg ⇒ CrossSection, else Scalar. Empty args ⇒ Scalar.
[[nodiscard]] inline Shape broadcast_max(std::span<const Shape> args) noexcept {
  Shape out = Shape::Scalar;
  for (const Shape s : args) {
    if (s == Shape::Panel) {
      return Shape::Panel; // Panel dominates; cannot grow further.
    }
    if (s == Shape::CrossSection) {
      out = Shape::CrossSection;
    }
  }
  return out;
}

} // namespace detail

// Element-wise / arithmetic / comparison / logical / select: broadcast to the
// maximum operand shape (Add/Sub/Mul/Div/MinP/MaxP/Pow/Spow/Select + cmp/logic).
[[nodiscard]] inline Shape shape_elementwise(std::span<const Shape> args) noexcept {
  return detail::broadcast_max(args);
}

// Unary element-wise (Neg/Abs/Sign/Log/Sigmoid/Tanh/Not): output shape == sole
// arg's shape.
// Defensive: empty args ⇒ Scalar (the checker rejects the arity before here).
[[nodiscard]] inline Shape shape_unary(std::span<const Shape> args) noexcept {
  return args.empty() ? Shape::Scalar : args.front();
}

// Cross-sectional ops (rank/zscore/scale/neutralize/group): logically P→V, so
// the output is always a CrossSection regardless of operand shapes.
[[nodiscard]] inline Shape shape_cross_section(std::span<const Shape> /*args*/) noexcept {
  return Shape::CrossSection;
}

// Time-series ops (all Ts*): P→P, output is always a Panel.
[[nodiscard]] inline Shape shape_panel(std::span<const Shape> /*args*/) noexcept {
  return Shape::Panel;
}

// =========================================================================
//  OpSig — one registry row per named operator/function.
// =========================================================================

// Maximum trailing-optional arguments any op may declare. Headroom: the
// largest optional-arg count among all planned ops is 1 (scale). Declared
// before OpSig because the struct embeds an array of this size.
inline constexpr atx::u8 kMaxDefaults = 2;

// A single output pin of a record (multi-output) op (P3d-B3).
// `name` is a string-literal view; `dtype` is the element type of that pin's
// output buffer. An OpSig with a non-empty `pins` span is a record op: its
// output is a named tuple rather than a single panel column.
struct PinSig {
  std::string_view name;
  DType dtype{DType::F64};
};

struct OpSig {
  std::string_view name;        // operator/function spelling (e.g. "ts_mean")
  atx::u8 min_arity{};          // REQUIRED operand count (== arity for fixed ops)
  atx::u8 max_arity{};          // max operand count; == min_arity for fixed-arity
  OpCode opcode{OpCode::Const}; // the VM instruction this op compiles to
  DType out_dtype{DType::F64};  // result element type
  bool lookahead_safe{true};    // causal rail; all built-ins are true
  // Trailing defaults for optional args: defaults[k] is the literal value
  // supplied for argument (min_arity + k) when the call omits it. Only the
  // first (max_arity - min_arity) entries are meaningful. A NaN sentinel means
  // "no scalar default" — the parser does NOT materialize that arg and the
  // op's kernel handles its absence.
  //
  // Invariant (register_op enforces the bound; callers must honor the ordering):
  //   * Optional arguments are TRAILING — required args occupy [0, min_arity),
  //     optionals occupy [min_arity, max_arity).
  //   * (max_arity - min_arity) <= kMaxDefaults (else register_op rejects).
  //   * A NaN sentinel marks "absent"; once an optional is absent, ALL later
  //     optionals are also absent — so a finite default must NOT follow a NaN
  //     sentinel (the parser stops materializing at the first NaN).
  std::array<atx::f64, kMaxDefaults> defaults{};
  // Non-owning, non-null pointer to a pure shape rule (plan §4). Given the
  // ordered child shapes, returns this op's output shape.
  Shape (*shape_of)(std::span<const Shape> args){nullptr};
  // ---- TRAILING fields added in P3d-B3 (member-defaults allow existing rows
  //      to omit them in aggregate init — the compiler fills them from the
  //      member-initializers here, so no existing row needs touching). ----
  //
  // Number of trailing arguments parsed as compile-time constant-literal
  // immediates (hyperparameters baked into the instruction at compile time).
  atx::u8 n_hparams{0};
  // Non-owning view into a static PinSig array. Empty = single output (the
  // normal case); non-empty = record op whose output is a named pin tuple.
  // Invariant (register_op enforces): either empty OR size >= 2.
  std::span<const PinSig> pins{};
};

// =========================================================================
//  Library — name → OpSig catalogue.
//
//  Built-ins (Appendix A) are registered at construction. Lookup is a linear
//  scan over a small table (the built-in set is a few dozen rows; parsing is a
//  cold path, so a flat array beats the cost/complexity of a hash map here).
// =========================================================================

class Library {
public:
  // Registers all built-in operators (Appendix A). Never throws: the built-in
  // table is statically valid (no duplicate names, every row has a shape_of).
  Library();

  // Look up an operator by name. Returns a non-owning pointer borrowed from the
  // Library, or nullptr if absent. Valid until the Library is destroyed/mutated.
  [[nodiscard]] const OpSig *find(std::string_view name) const noexcept;

  // Register a user operator. Err(InvalidArgument) on an empty name or a null
  // shape_of; Err(AlreadyExists) on a duplicate name; Ok otherwise.
  atx::core::Status register_op(const OpSig &sig);

  // Number of registered operators (built-ins + user ops).
  [[nodiscard]] atx::usize size() const noexcept { return ops_.size(); }

private:
  std::vector<OpSig> ops_;
};

// =========================================================================
//  Built-in table.
//
//  One row per *named* operator/function in Appendix A. Infix operators
//  (+ - * / ^ < > <= >= == != && ||) are dispatched by the parser's ParseRule
//  table straight to their OpCodes and so need NOT appear here — but the named
//  functions MUST. Out dtype is F64 except comparisons/logical (Mask); all
//  built-ins are lookahead_safe.
// =========================================================================

// Static pin table for the split2 test-only builtin (P3d-B3). Two named
// output pins: "hi" (high-pass) and "lo" (low-pass), both F64 panel signals.
// Lifetime: static storage — the std::span<const PinSig> in the OpSig row
// borrows from here; the borrow is non-dangling (program lifetime).
inline constexpr std::array<PinSig, 2> kSplit2Pins = {{{"hi", DType::F64}, {"lo", DType::F64}}};

// Static pin table for kalman(y,x,delta,R) (P3d-D2). Three named output pins:
// "alpha" (time-varying intercept), "beta" (time-varying slope), "resid"
// (standardised innovation e/sqrt(Q)). All F64 Panel signals.
// Lifetime: static storage — non-dangling for the program lifetime.
inline constexpr std::array<PinSig, 3> kKalmanRegPins = {
    {{"alpha", DType::F64}, {"beta", DType::F64}, {"resid", DType::F64}}};

namespace detail {

// The complete built-in catalogue (Appendix A named functions). Kept as a
// static span so construction is a single copy. `consteval`-friendly literals;
// the array has static storage, so every `name` view is non-dangling forever.
[[nodiscard]] std::span<const OpSig> builtin_ops() noexcept;

} // namespace detail

// =========================================================================
//  Library member definitions (inline; header-only).
// =========================================================================

inline const OpSig *Library::find(std::string_view name) const noexcept {
  for (const OpSig &op : ops_) {
    if (op.name == name) {
      return &op;
    }
  }
  return nullptr;
}

} // namespace atx::engine::alpha
