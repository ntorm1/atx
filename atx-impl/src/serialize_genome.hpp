#pragma once

// atx::impl — genome (de)serialization as DSL source strings.
//
// A factory::Genome holds an alpha::Ast whose Expr::op is a non-owning
// const OpSig* borrowed from the run-wide Library — NOT serializable as a
// raw pointer.  The engine guarantees:
//
//     canonical_hash(parse_expr(unparse(ast))) == canonical_hash(ast)
//
// So we serialize as a DSL text string (one line) and deserialize by
// parse_expr + analyze_into, which re-resolves Expr::op against the Library
// and re-derives the Analysis.

#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/parser.hpp"   // alpha::Library
#include "atx/engine/factory/genome.hpp" // factory::Genome

namespace atx::impl {

// Write a genome's DSL form (unparse of its single root) to `path` (one
// line + '\n').  Err(InvalidArgument) if the ofstream cannot be opened.
[[nodiscard]] atx::core::Status
write_genome(const atx::engine::factory::Genome& g, const std::string& path);

// Read a .dsl file, parse_expr against `lib`, analyze_into -> Genome.
// Err(ParseError) if the file cannot be read or parse fails; propagates
// analyze_into errors.
[[nodiscard]] atx::core::Result<atx::engine::factory::Genome>
read_genome(const std::string& path,
            const atx::engine::alpha::Library& lib);

} // namespace atx::impl
