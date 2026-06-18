#include "serialize_genome.hpp"

#include <algorithm> // std::find_if
#include <fstream>
#include <string>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/parser.hpp"   // alpha::parse_expr, alpha::Library
#include "atx/engine/alpha/unparse.hpp"  // alpha::unparse
#include "atx/engine/factory/genome.hpp" // factory::analyze_into, factory::Genome

namespace atx::impl {

atx::core::Status
write_genome(const atx::engine::factory::Genome& g, const std::string& path)
{
    std::ofstream ofs{path};
    if (!ofs.is_open()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "write_genome: cannot open file: " + path);
    }
    const std::string s = atx::engine::alpha::unparse(g.ast);
    ofs << s << '\n';
    return atx::core::Ok();
}

atx::core::Result<atx::engine::factory::Genome>
read_genome(const std::string& path,
            const atx::engine::alpha::Library& lib)
{
    std::ifstream ifs{path};
    if (!ifs.is_open()) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_genome: cannot open file: " + path);
    }

    // Read entire file into a string.
    std::string text{std::istreambuf_iterator<char>{ifs},
                     std::istreambuf_iterator<char>{}};

    // Trim trailing whitespace / newlines.
    const auto last = std::find_if(text.rbegin(), text.rend(),
                                   [](unsigned char c) { return !std::isspace(c); });
    text.erase(last.base(), text.end());

    // Parse the DSL string back into an Ast.
    auto ast_result = atx::engine::alpha::parse_expr(text, lib);
    if (!ast_result.has_value()) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_genome: parse failed for '" + path +
                              "': " + ast_result.error().message());
    }

    // Re-derive Analysis and package into a Genome (F5 validity backstop).
    return atx::engine::factory::analyze_into(std::move(*ast_result));
}

} // namespace atx::impl
