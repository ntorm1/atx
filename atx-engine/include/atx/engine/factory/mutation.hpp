#pragma once

// atx::engine::factory — mutation operators (S3-1, plan §4.2 / §0.2 / §0.3).
//
// Three type-safe, seeded, single-point mutations over a Genome. Each:
//   1. enumerates its candidate targets in CANONICAL-ID (ascending ExprId)
//      order — the fixed order is established BEFORE any RNG draw (F1/F2);
//   2. draws ONE target (and one replacement) from a seeded `Xoshiro256pp`;
//   3. REBUILDS a fresh Ast via `rebuild_with` (the Ast has no in-place edit);
//   4. funnels through `analyze_into` — the F5 validity oracle. A candidate that
//      fails analyze (shape/dtype/causality) is returned as `Err`, never a
//      half-valid genome. `Err(NotFound)` when no target/replacement exists.
//
//   op_swap     — swap a Call node's named op for another in the same
//                 (shape, dtype, arity) OpCatalog bucket (!= current op).
//   field_swap  — swap a Field leaf for another panel field (same F64 slot).
//   jitter_const— perturb a Literal, classified Window vs Scale vs Hparam (§0.3):
//                 Window (the trailing integer operand of a Ts* op) is jittered
//                 multiplicatively then floored & clamped to [1, max_lookback];
//                 Scale (any other numeric literal) is jittered multiplicatively.
//
// Determinism (F1): every draw is from the caller-seeded `Xoshiro256pp`; same
// seed ⇒ byte-identical mutant. NEVER seed by worker/thread/time.
//
// Header-only; COLD path (one rebuild per candidate). SAFETY: the OpCatalog and
// every genome borrow ops from the ONE run-wide Library (see genome.hpp).

#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"

#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/op_catalog.hpp"

namespace atx::engine::factory {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::call_arity;
using atx::engine::alpha::DType;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::OpSig;

// =========================================================================
//  Literal classification (§0.3) — Window vs Scale vs Hparam.
// =========================================================================

enum class ConstKind : atx::u8 { Window, Scale, Hparam };

struct ClassifiedConst {
  ExprId id{kNoExpr};
  ConstKind kind{ConstKind::Scale};
};

// Tuning for jitter_const. `sigma` is the log-normal step (multiplicative);
// `max_lookback` is the inclusive upper bound a jittered Window is clamped to.
struct JitterCfg {
  atx::f64 sigma{0.5};
  atx::u16 max_lookback{250};
};

// Walk the arena and classify every Literal node. A Literal is:
//   * Window — iff it is the trailing window operand of a temporal Ts* Call
//     (arg `c` for arity-3 corr/cov, else arg `b`; the operand `window_value`
//     reads in the type-checker);
//   * Hparam — iff it is an ORPHAN literal whose value equals a Call's peeled
//     `hparams[k]` (the parser peels trailing hparam args into Expr::hparams and
//     leaves the source Literal node unreferenced by any a/b/c, §0.3);
//   * Scale  — any other numeric literal (the fractional-constant / coefficient).
//
// Returned in ascending-ExprId (canonical) order so a downstream seeded draw is
// deterministic (F1).
[[nodiscard]] std::vector<ClassifiedConst> classify_literals(const Genome &g);

// =========================================================================
//  op_swap — swap a Call node's named op (§4.2).
// =========================================================================

[[nodiscard]] atx::core::Result<Genome> op_swap(const Genome &g, const OpCatalog &cat,
                                                Xoshiro256pp &rng);

// =========================================================================
//  field_swap — swap a Field leaf for another panel field (§4.2).
// =========================================================================

[[nodiscard]] atx::core::Result<Genome>
field_swap(const Genome &g, std::span<const std::string_view> panel_fields, Xoshiro256pp &rng);

// =========================================================================
//  jitter_const — perturb a classified Literal (§4.2 / §0.3).
// =========================================================================

[[nodiscard]] atx::core::Result<Genome> jitter_const(const Genome &g, Xoshiro256pp &rng,
                                                     JitterCfg cfg);

} // namespace atx::engine::factory
