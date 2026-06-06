#pragma once

// atx::engine::alpha — alpha-expression DSL forward declarations (Phase 3).
//
// A lightweight header other engine headers include to NAME the DSL spine
// types without pulling in their full definitions (and the lexer, parser,
// DAG compiler, and VM machinery behind them). Keeping the forward set here
// means a header that only passes an `alpha::Engine*` or a `alpha::Program&`
// around does not transitively include the expression tree, the hash-consed
// DAG, or the columnar eval context.
//
// Full definitions live in (added per phase unit):
//   alpha/lexer.hpp    — TokenKind, Token, Span                       (P3-1)
//   alpha/parser.hpp   — Expr, Library, OpSig                         (P3-2)
//   alpha/typecheck.hpp — (uses Token/Expr; no new forward types)     (P3-3)
//   alpha/dag.hpp      — Node, Dag                                    (P3-4)
//   alpha/bytecode.hpp — OpCode, Program                              (P3-4)
//   alpha/vm.hpp       — SignalSet                                     (P3-6)
//   alpha/engine.hpp   — Engine                                       (P3-9)
//
// NOTE: OpCode, TokenKind, Shape, and DType are scoped enums with an explicit
// underlying type (atx::u8). They are forward-declared here so callers that
// store or pass these values by type can include only this header rather than
// the full definition headers.

#include "atx/core/types.hpp" // atx::u8 (needed for enum underlying types)

namespace atx::engine::alpha {

// =====================================================================
//  Scoped enums — forward declarations with explicit underlying type
// =====================================================================

// Lexer token category. Full definition in alpha/lexer.hpp (P3-1).
enum class TokenKind : atx::u8;

// VM instruction opcode. Full definition in alpha/bytecode.hpp (P3-4).
enum class OpCode : atx::u8;

// Signal shape: Scalar (S), Vector/cross-sectional (V), or Panel (P).
// Full definition in alpha/typecheck.hpp (P3-3).
enum class Shape : atx::u8;

// Element data type: f64, mask (boolean), or group (integer label).
// Full definition in alpha/typecheck.hpp (P3-3).
enum class DType : atx::u8;

// =====================================================================
//  Lexer — token stream (P3-1)
// =====================================================================

// A single lexed token: kind + byte range into the source string.
// Full definition in alpha/lexer.hpp (P3-1).
struct Token;

// Half-open byte range [begin, end) into a source-string view.
// Used by Token and by parser diagnostics. Full definition in alpha/lexer.hpp (P3-1).
struct Span;

// =====================================================================
//  Parser / registry — AST + operator catalogue (P3-2)
// =====================================================================

// An immutable expression tree node produced by the Pratt parser. Carries
// shape/dtype annotations after P3-3 typecheck. Full definition in alpha/parser.hpp (P3-2).
struct Expr;

// Operator registry: maps name → OpSig; supports const-fold and desugar
// rewrites. Full definition in alpha/parser.hpp (P3-2).
class Library;

// Operator signature: arity, shape/dtype rules, lookback.
// Full definition in alpha/parser.hpp (P3-2).
struct OpSig;

// =====================================================================
//  DAG compiler — hash-consed expression graph + bytecode (P3-4)
// =====================================================================

// A single node in the hash-consed expression DAG (free CSE via structural
// equality). Full definition in alpha/dag.hpp (P3-4).
struct Node;

// The hash-consed directed-acyclic expression graph. Structurally identical
// sub-expressions share one Node; the DAG owns all nodes.
// Full definition in alpha/dag.hpp (P3-4).
class Dag;

// Linearized bytecode program produced from a Dag topo-sort: an Instr
// stream + slot map + refcount Free instructions.
// Full definition in alpha/bytecode.hpp (P3-4).
struct Program;

// =====================================================================
//  VM / eval context (P3-6 .. P3-9)
// =====================================================================

// Output of one Program execution over a Panel: one f64 column per
// instrument, NaN where the mask is false. Full definition in alpha/vm.hpp (P3-6).
struct SignalSet;

// Top-level DSL entry point: parse → typecheck → compile → execute.
// Owns the Library registry and a cached Program pool.
// Full definition in alpha/engine.hpp (P3-9).
class Engine;

} // namespace atx::engine::alpha
