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
#include <string>
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
  // ---- stateful recurrence (P3b-3): carry TRUE cross-date state from the
  //      panel's first date forward (no trailing window). Causal by
  //      construction — the forward scan reads only state[t-1] + inputs <= t.
  TradeWhen,
  Hump,
  // ---- multi-output / record ops (P3d-B3): split a panel into named pins.
  //      Kernels land in B9; these enumerators exist now so the ISA is
  //      forward-declared and the registry + switch sites stay green.
  Pin,
  Split2,
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

namespace detail {

// The complete built-in catalogue (Appendix A named functions). Kept as a
// static span so construction is a single copy. `consteval`-friendly literals;
// the array has static storage, so every `name` view is non-dangling forever.
[[nodiscard]] inline std::span<const OpSig> builtin_ops() noexcept {
  // Rows are positional: {name, min_arity, max_arity, opcode, out_dtype,
  // lookahead_safe, defaults, shape_of}. Fixed-arity ops carry min==max and an
  // empty defaults array. `scale` is the lone variadic built-in in 3b: 1
  // required arg, 1 optional with a finite default of 1.0 (P3b-1).
  // P3d-B3 adds two trailing OpSig fields (n_hparams, pins) with member-
  // initializers — existing rows omit them and pick up {0, {}} automatically.
  static constexpr std::array<OpSig, 58> kOps = {{
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
  }};
  return kOps;
}

} // namespace detail

// =========================================================================
//  Library member definitions (inline; header-only).
// =========================================================================

inline Library::Library() {
  const std::span<const OpSig> builtins = detail::builtin_ops();
  ops_.assign(builtins.begin(), builtins.end());
}

inline const OpSig *Library::find(std::string_view name) const noexcept {
  for (const OpSig &op : ops_) {
    if (op.name == name) {
      return &op;
    }
  }
  return nullptr;
}

inline atx::core::Status Library::register_op(const OpSig &sig) {
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
