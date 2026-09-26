#include "prereg.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <ios>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/lexer.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/unparse.hpp"

namespace atx::impl {
namespace {
using Json = nlohmann::json;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
constexpr atx::usize kMaxBytes = 1024U * 1024U;
constexpr atx::usize kMaxFamilies = 64;

bool digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void require(bool valid, const char *message) {
    if (!valid) throw std::invalid_argument(message);
}
void keys(const Json &value, std::initializer_list<std::string_view> expected) {
    require(value.is_object() && value.size() == expected.size(), "object keys differ");
    for (const auto key : expected) require(value.contains(std::string(key)), "unknown/missing key");
}
std::string text(const Json &value, atx::usize maximum) {
    require(value.is_string(), "expected string");
    auto out = value.get<std::string>();
    require(!out.empty() && out.size() <= maximum &&
            out.find_first_of("\r\n\t") == std::string::npos &&
            out.find('\0') == std::string::npos, "invalid string size/control characters");
    return out;
}
std::string identifier(const Json &value, atx::usize maximum = 96) {
    auto out = text(value, maximum);
    require(std::all_of(out.begin(), out.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    }), "unsafe identifier");
    return out;
}
atx::i64 integer(const Json &value, atx::i64 low, atx::i64 high) {
    require(value.is_number_integer(), "expected integer");
    if (value.is_number_unsigned()) {
        require(value.get<atx::u64>() <= static_cast<atx::u64>(high), "integer out of range");
    }
    const auto out = value.get<atx::i64>();
    require(out >= low && out <= high, "integer out of range");
    return out;
}
} // namespace

Result<EquityIcPrereg> parse_equity_ic_prereg(std::string_view source) {
    if (source.empty() || source.size() > kMaxBytes) {
        return Err(ErrorCode::OutOfRange, "prereg: file must contain 1..1048576 bytes");
    }
    try {
        std::vector<std::set<std::string>> objects;
        auto root = Json::parse(source, [&objects](int depth, Json::parse_event_t event,
                                                        Json &value) {
            require(depth <= 12, "JSON nesting exceeds limit");
            if (event == Json::parse_event_t::object_start) objects.emplace_back();
            if (event == Json::parse_event_t::key) {
                require(!objects.empty() && objects.back().insert(value.get<std::string>()).second,
                        "duplicate key");
            }
            if (event == Json::parse_event_t::object_end) objects.pop_back();
            return true;
        });
        keys(root, {"schema", "epoch", "checkpoint", "declared_n", "forward_variants",
                    "restrictions", "families"});
        require(root.at("schema") == "atx-equity-ic-prereg-v1", "unknown schema");
        require(root.at("forward_variants") ==
                    Json::array({"DropMissingForward", "IncludeAuditedTerminalV1"}),
                "unsupported forward variants/order");
        require(root.at("restrictions") == Json::array({"full", "_ex34"}),
                "unsupported restrictions/order");
        EquityIcPrereg out;
        out.epoch = identifier(root.at("epoch"));
        out.checkpoint = integer(root.at("checkpoint"), 1, 2147483647);
        out.declared_n = integer(root.at("declared_n"), 0, 64 * 8 * 4);
        auto &families = root.at("families");
        require(families.is_array() && !families.empty() && families.size() <= kMaxFamilies,
                "family count must be 1..64");
        std::set<std::string> ids, names, configurations;
        std::set<atx::usize> horizons;
        atx::i64 declared = 0;
        const atx::engine::alpha::Library library;
        for (auto &row : families) {
            keys(row, {"id", "name", "dsl", "sign", "theme", "horizons", "lineage"});
            PreregFamily family;
            family.id = identifier(row.at("id"));
            family.name = identifier(row.at("name"));
            require(ids.insert(family.id).second && names.insert(family.name).second,
                    "duplicate family ID/name");
            require(family.name != "momentum_252" && family.name != "momentum_126" &&
                    family.name != "blend_equal" && family.name != "dollar_adv_21",
                    "reserved reference name");
            family.dsl = text(row.at("dsl"), 4096);
            ATX_TRY(auto tokens, atx::engine::alpha::lex(family.dsl));
            require(tokens.size() <= 256, "DSL exceeds 256-token recursion/work bound");
            ATX_TRY(auto ast, atx::engine::alpha::parse_expr(family.dsl, library));
            for (const auto &node : ast.nodes()) {
                require(node.kind != atx::engine::alpha::Expr::Kind::Literal ||
                            std::isfinite(node.value), "nonfinite folded DSL literal");
            }
            family.dsl = atx::engine::alpha::unparse(ast);
            row["dsl"] = family.dsl;
            family.theme = text(row.at("theme"), 128);
            family.sign = static_cast<int>(integer(row.at("sign"), -1, 1));
            require(family.sign != 0, "sign must be -1 or 1");
            const auto &requested = row.at("horizons");
            require(requested.is_array() && !requested.empty() && requested.size() <= 8,
                    "horizon count must be 1..8");
            for (const auto &h : requested) {
                const auto value = static_cast<atx::usize>(integer(h, 1, 4096));
                require(family.horizons.empty() || family.horizons.back() < value,
                        "horizons must be strictly increasing");
                family.horizons.push_back(value);
                horizons.insert(value);
            }
            // Identity includes all numerical choices and family labels, with DSL text
            // canonicalized by the existing alpha AST printer under this schema.
            Json configuration = row;
            configuration.erase("lineage");
            configuration["forward_variants"] = root.at("forward_variants");
            configuration["restrictions"] = root.at("restrictions");
            ATX_TRY(family.configuration_sha256,
                    atx::core::sha256_hex("atx-ic-family-v1\n" + configuration.dump()));
            Json numerical = configuration;
            numerical.erase("id");
            numerical.erase("name");
            numerical.erase("theme");
            numerical.erase("horizons");
            for (const auto h : family.horizons) {
                numerical["horizon"] = h;
                require(configurations.insert(numerical.dump()).second,
                        "overlapping numerical family configuration");
            }
            const auto &lineage = row.at("lineage");
            require(lineage.is_object() && lineage.contains("kind"), "missing lineage");
            if (lineage.at("kind") == "new") {
                keys(lineage, {"kind"});
                declared += static_cast<atx::i64>(family.horizons.size() * 4);
            } else {
                keys(lineage, {"kind", "prereg_sha256", "configuration_sha256", "trial_id"});
                require(lineage.at("kind") == "retained", "unknown lineage kind");
                require(digest(text(lineage.at("prereg_sha256"), 64)), "invalid lineage digest");
                require(text(lineage.at("configuration_sha256"), 64) ==
                            family.configuration_sha256, "retained configuration changed");
                (void)identifier(lineage.at("trial_id"), 256);
                family.retained = true;
                ++out.retained_count;
            }
            out.measured_n += static_cast<atx::i64>(family.horizons.size() * 4);
            out.families.push_back(std::move(family));
        }
        require(horizons.size() <= 8, "union of horizons exceeds engine limit 8");
        require(declared == out.declared_n, "declared_n differs from declared new configurations");
        out.horizons.assign(horizons.begin(), horizons.end());
        out.canonical_json = root.dump();
        ATX_TRY(out.canonical_sha256,
                atx::core::sha256_hex("atx-equity-ic-prereg-v1\n" + out.canonical_json));
        ATX_TRY(out.file_sha256, atx::core::sha256_hex(source));
        return Ok(std::move(out));
    } catch (const std::exception &error) {
        return Err(ErrorCode::InvalidArgument, "prereg: " + std::string(error.what()));
    }
}

Result<EquityIcPrereg> load_equity_ic_prereg(const std::string &path,
                                           std::string_view expected_file_sha256) {
    if (!digest(expected_file_sha256)) {
        return Err(ErrorCode::InvalidArgument, "prereg: expected lowercase SHA256 required");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return Err(ErrorCode::IoError, "prereg: cannot open file");
    std::string source(kMaxBytes + 1, '\0');
    input.read(source.data(), static_cast<std::streamsize>(source.size()));
    if (input.bad()) return Err(ErrorCode::IoError, "prereg: file read failed");
    source.resize(static_cast<atx::usize>(input.gcount()));
    if (source.size() > kMaxBytes) return Err(ErrorCode::OutOfRange, "prereg: file too large");
    ATX_TRY(auto actual, atx::core::sha256_hex(source));
    if (actual != expected_file_sha256) {
        return Err(ErrorCode::InvalidArgument, "prereg: exact file SHA256 mismatch");
    }
    return parse_equity_ic_prereg(source);
}
} // namespace atx::impl
