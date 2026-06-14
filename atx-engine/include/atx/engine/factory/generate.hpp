#pragma once

// atx::engine::factory — grammar-typed (valid-by-construction) generation (S3.5).
//
// A recursive shape/dtype-targeted sampler: given a target DType (F64 / Mask /
// Group) and a depth budget, emit a DSL expression of EXACTLY that type by
// sampling only productions whose result matches the target and recursing into
// each operand at the operand's required type. Because every node is sampled
// against the type lattice (Scalar < CrossSection < Panel; F64 / Mask / Group)
// and every operand role (group classifier, window literal, scalar) is honored,
// the emitted string is analyze-VALID BY CONSTRUCTION — its `analyze` rejection
// rate is ≈ 0 (the unit's headline, measured against the type-blind control
// below, whose rejection rate is high).
//
// We emit a DSL STRING and round-trip through the real parser + type-checker
// rather than hand-building an Ast: the parser is the single source of truth for
// syntax, and the round-trip is what the rejection-rate metric measures. Leaves
// are panel fields (numeric → F64 Panel, `IndClass.*` → Group) or literals
// (integer windows in [1, max_lookback]; scalar factors). hparam / record ops
// are intentionally excluded (their peeled-immediate contract needs special
// handling — §0.3); every op emitted here is a plain operand-only op, so the
// generated genome is well-formed without hparam gymnastics.
//
// Determinism: the single Xoshiro256pp is the sole entropy; same seed ⇒ same
// genome (composes with S4's seed axis). Header-only; a COLD path.

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

#include "atx/engine/factory/genome.hpp"

namespace atx::engine::factory {

using atx::core::Xoshiro256pp;

// Generation knobs. The field catalogs default to the canonical OHLCV + S3.3
// datafields (numeric) and the IndClass classifiers (Group); a caller may
// override them to match a specific panel.
struct GenConfig {
  atx::u16 max_lookback{60};
  int max_depth{4};
  std::vector<std::string_view> numeric_fields{"close", "open",   "high", "low",
                                               "volume", "vwap", "adv20"};
  std::vector<std::string_view> group_fields{"IndClass.sector", "IndClass.industry",
                                             "IndClass.subindustry"};
};

namespace detail {

[[nodiscard]] inline std::string_view pick_sv(std::span<const std::string_view> xs,
                                              Xoshiro256pp &rng) {
  return xs[static_cast<atx::usize>(rng.next_u64() % xs.size())];
}

[[nodiscard]] inline std::string_view pick_field(const std::vector<std::string_view> &xs,
                                                 Xoshiro256pp &rng) {
  return xs[static_cast<atx::usize>(rng.next_u64() % xs.size())];
}

[[nodiscard]] inline std::string emit_window(const GenConfig &cfg, Xoshiro256pp &rng) {
  const atx::u64 w = 1 + (rng.next_u64() % cfg.max_lookback);
  return std::to_string(w);
}

[[nodiscard]] inline std::string emit_scalar(Xoshiro256pp &rng) {
  static constexpr std::array<std::string_view, 4> kScalars = {{"0.5", "1.5", "2.0", "3.0"}};
  return std::string{pick_sv(kScalars, rng)};
}

// ----- typed grammar (mutually recursive) -------------------------------------

[[nodiscard]] std::string gen_f64(const GenConfig &cfg, Xoshiro256pp &rng, int depth);
[[nodiscard]] std::string gen_mask(const GenConfig &cfg, Xoshiro256pp &rng, int depth);

// A Group value is ONLY ever a classifier field (no op produces Group).
[[nodiscard]] inline std::string gen_group(const GenConfig &cfg, Xoshiro256pp &rng) {
  return std::string{pick_field(cfg.group_fields, rng)};
}

[[nodiscard]] inline std::string gen_mask(const GenConfig &cfg, Xoshiro256pp &rng, int depth) {
  static constexpr std::array<std::string_view, 6> kCmp = {{"<", ">", "<=", ">=", "==", "!="}};
  static constexpr std::array<std::string_view, 2> kLogic = {{"&&", "||"}};
  // A logical node needs Mask operands; a comparison needs only F64 operands, so
  // at depth 0 we still can produce a Mask from two field comparisons.
  if (depth <= 0 || (rng.next_u64() % 2) == 0) {
    return "(" + gen_f64(cfg, rng, depth - 1) + " " + std::string{pick_sv(kCmp, rng)} + " " +
           gen_f64(cfg, rng, depth - 1) + ")";
  }
  return "(" + gen_mask(cfg, rng, depth - 1) + " " + std::string{pick_sv(kLogic, rng)} + " " +
         gen_mask(cfg, rng, depth - 1) + ")";
}

[[nodiscard]] inline std::string gen_f64(const GenConfig &cfg, Xoshiro256pp &rng, int depth) {
  // Terminate at the budget, or stochastically, with a numeric panel field (a
  // Panel F64 leaf — never a bare scalar, so a Cs*/Ts* primary stays non-scalar).
  if (depth <= 0 || (rng.next_u64() % 4) == 0) {
    return std::string{pick_field(cfg.numeric_fields, rng)};
  }
  static constexpr std::array<std::string_view, 6> kUnary = {
      {"abs", "sign", "log", "sigmoid", "tanh", "reverse"}};
  static constexpr std::array<std::string_view, 4> kArith = {{"+", "-", "*", "/"}};
  static constexpr std::array<std::string_view, 5> kCsSimple = {
      {"rank", "zscore", "normalize", "vec_sum", "vec_avg"}};
  static constexpr std::array<std::string_view, 3> kCsScalar = {{"scale", "winsorize", "quantile"}};
  static constexpr std::array<std::string_view, 7> kCsGroup = {
      {"indneutralize", "group_rank", "group_zscore", "group_mean", "group_count", "group_scale",
       "cs_residualize"}};
  static constexpr std::array<std::string_view, 10> kTsUnary = {
      {"ts_mean", "ts_std", "ts_sum", "ts_min", "ts_max", "delay", "delta", "ts_rank",
       "decay_linear", "ema"}};
  static constexpr std::array<std::string_view, 2> kTsBinary = {{"correlation", "covariance"}};

  switch (rng.next_u64() % 8) {
  case 0: // unary element-wise function
    return std::string{pick_sv(kUnary, rng)} + "(" + gen_f64(cfg, rng, depth - 1) + ")";
  case 1: // binary arithmetic (infix)
    return "(" + gen_f64(cfg, rng, depth - 1) + " " + std::string{pick_sv(kArith, rng)} + " " +
           gen_f64(cfg, rng, depth - 1) + ")";
  case 2: // cross-sectional, arity 1
    return std::string{pick_sv(kCsSimple, rng)} + "(" + gen_f64(cfg, rng, depth - 1) + ")";
  case 3: // cross-sectional with a scalar 2nd operand
    return std::string{pick_sv(kCsScalar, rng)} + "(" + gen_f64(cfg, rng, depth - 1) + ", " +
           emit_scalar(rng) + ")";
  case 4: // group-aware cross-sectional (2nd operand is a Group classifier)
    return std::string{pick_sv(kCsGroup, rng)} + "(" + gen_f64(cfg, rng, depth - 1) + ", " +
           gen_group(cfg, rng) + ")";
  case 5: // time-series, arity 2 (window literal)
    return std::string{pick_sv(kTsUnary, rng)} + "(" + gen_f64(cfg, rng, depth - 1) + ", " +
           emit_window(cfg, rng) + ")";
  case 6: // time-series, arity 3 (two panels + window literal)
    return std::string{pick_sv(kTsBinary, rng)} + "(" + gen_f64(cfg, rng, depth - 1) + ", " +
           gen_f64(cfg, rng, depth - 1) + ", " + emit_window(cfg, rng) + ")";
  default: // prefix negate
    return "(-" + gen_f64(cfg, rng, depth - 1) + ")";
  }
}

// ----- type-BLIND control (the rejection baseline) ----------------------------
//
// Emits a syntactically-valid but type-IGNORANT tree: every internal node picks
// a random op and fills its operands with random leaves regardless of the op's
// required dtype / role / window-constant rail. So a group op may receive a
// numeric field, an arithmetic op a Mask, a window slot a non-constant — exactly
// the violations the type-checker rejects. (Syntax stays well-formed so the
// rejection isolates the TYPE-checker, the apples-to-apples comparison.)
[[nodiscard]] inline std::string gen_control(const GenConfig &cfg, Xoshiro256pp &rng, int depth) {
  if (depth <= 0 || (rng.next_u64() % 3) == 0) {
    switch (rng.next_u64() % 4) {
    case 0:
      return std::string{pick_field(cfg.numeric_fields, rng)};
    case 1:
      return std::string{pick_field(cfg.group_fields, rng)}; // Group leaf in an F64 slot
    case 2:
      return emit_scalar(rng); // a bare scalar (mis-shapes a Cs*/Ts* primary)
    default:
      return "(" + std::string{pick_field(cfg.numeric_fields, rng)} + " > " +
             std::string{pick_field(cfg.numeric_fields, rng)} + ")"; // a Mask leaf
    }
  }
  // A flat op pool with FIXED arities but type-blind operands.
  static constexpr std::array<std::string_view, 8> kAny1 = {
      {"abs", "rank", "zscore", "normalize", "log", "sigmoid", "vec_sum", "reverse"}};
  static constexpr std::array<std::string_view, 6> kAny2 = {
      {"ts_mean", "indneutralize", "scale", "delay", "group_rank", "winsorize"}};
  switch (rng.next_u64() % 3) {
  case 0:
    return std::string{pick_sv(kAny1, rng)} + "(" + gen_control(cfg, rng, depth - 1) + ")";
  case 1: // 2nd operand is a random sub-expression (often the wrong role / non-constant)
    return std::string{pick_sv(kAny2, rng)} + "(" + gen_control(cfg, rng, depth - 1) + ", " +
           gen_control(cfg, rng, depth - 1) + ")";
  default: // arithmetic over random-typed operands
    return "(" + gen_control(cfg, rng, depth - 1) + " + " + gen_control(cfg, rng, depth - 1) + ")";
  }
}

} // namespace detail

// Emit a type-correct DSL expression string of result dtype F64 (a tradeable
// Panel/CrossSection signal).
[[nodiscard]] inline std::string generate_expr(const GenConfig &cfg, Xoshiro256pp &rng) {
  return detail::gen_f64(cfg, rng, cfg.max_depth);
}

// Emit a type-BLIND control expression string (the rejection baseline).
[[nodiscard]] inline std::string generate_control_expr(const GenConfig &cfg, Xoshiro256pp &rng) {
  return detail::gen_control(cfg, rng, cfg.max_depth);
}

// Generate a type-correct genome: parse + analyze the typed string. Ok by
// construction (an Err signals a generator bug, not a search rejection). The
// `lib` must outlive the returned genome (its nodes borrow OpSig* from it).
[[nodiscard]] inline atx::core::Result<Genome>
generate_genome(const GenConfig &cfg, const alpha::Library &lib, Xoshiro256pp &rng) {
  const std::string src = generate_expr(cfg, rng);
  ATX_TRY(alpha::Ast ast, alpha::parse_expr(src, lib));
  ATX_TRY(alpha::Analysis ana, alpha::analyze(ast));
  return atx::core::Ok(Genome{std::move(ast), std::move(ana), 0});
}

} // namespace atx::engine::factory
