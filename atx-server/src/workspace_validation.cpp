#include "workspace_validation.hpp"
#include <atx/server/core.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <vector>

namespace atx::server::workspace_detail {
namespace {
struct Character { std::uint32_t value; std::size_t begin; };
std::vector<Character> characters(std::string_view text) {
    std::vector<Character> result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto begin = i;
        const auto first = static_cast<unsigned char>(text[i++]);
        auto value = static_cast<std::uint32_t>(first);
        auto following = 0U;
        if (first >= 0xc2U && first <= 0xdfU) { following = 1; value &= 0x1fU; }
        else if (first >= 0xe0U && first <= 0xefU) { following = 2; value &= 0x0fU; }
        else if (first >= 0xf0U && first <= 0xf4U) { following = 3; value &= 0x07U; }
        else if (first >= 0x80U) throw Error(ErrorCode::invalid, "workspace text must be UTF-8");
        if (following > text.size() - i) throw Error(ErrorCode::invalid, "incomplete UTF-8 text");
        for (unsigned j = 0; j < following; ++j) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0U) != 0x80U) throw Error(ErrorCode::invalid, "invalid UTF-8 text");
            value = (value << 6U) | (byte & 0x3fU);
        }
        constexpr std::uint32_t minimum[]{0, 0x80, 0x800, 0x10000};
        if (value < minimum[following] || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) {
            throw Error(ErrorCode::invalid, "invalid UTF-8 code point");
        }
        result.push_back({value, begin});
    }
    return result;
}
bool whitespace(std::uint32_t cp) {
    return (cp >= 9 && cp <= 13) || cp == 32 || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
        (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f ||
        cp == 0x205f || cp == 0x3000;
}
std::string validated_name(std::string_view text) {
    if (text.size() > 320) throw Error(ErrorCode::invalid, "workspace name exceeds 320 bytes");
    const auto decoded = characters(text);
    auto first = std::size_t{};
    auto last = decoded.size();
    for (; first < last && whitespace(decoded[first].value); ++first) {}
    for (; last > first && whitespace(decoded[last - 1].value); --last) {}
    if (last == first || last - first > 80) {
        throw Error(ErrorCode::invalid, "workspace name must contain 1 to 80 characters");
    }
    for (auto i = first; i < last; ++i) {
        const auto cp = decoded[i].value;
        if (cp < 32 || (cp >= 0x7f && cp <= 0x9f)) {
            throw Error(ErrorCode::invalid, "workspace name contains a control character");
        }
    }
    const auto end = last == decoded.size() ? text.size() : decoded[last].begin;
    return std::string(text.substr(decoded[first].begin, end - decoded[first].begin));
}
void bound_nesting(std::string_view text) {
    auto depth = 0;
    auto quoted = false;
    auto escaped = false;
    for (const auto ch : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > 32) throw Error(ErrorCode::invalid, "workspace JSON nesting exceeds 32");
        } else if (ch == '}' || ch == ']') --depth;
    }
}
std::string validated_document(std::string_view text) {
    const auto json = parse_json(text, 96 * 1024);
    if (json.is_discarded() || !json.is_object() || !json.contains("schema_version") ||
        !json["schema_version"].is_number_integer() || json["schema_version"] != 1 ||
        !json.contains("layout") || !json["layout"].is_object() ||
        (json.contains("panel_state") && !json["panel_state"].is_object())) {
        throw Error(ErrorCode::invalid, "workspace requires schema_version=1 and layout object");
    }
    for (const auto& item : json.items()) {
        if (item.key() != "schema_version" && item.key() != "layout" &&
            item.key() != "panel_state") {
            throw Error(ErrorCode::invalid, "unsupported workspace envelope field");
        }
    }
    auto canonical = json.dump();
    if (canonical.size() > 96 * 1024) {
        throw Error(ErrorCode::invalid, "canonical workspace is too large");
    }
    return canonical;
}
} // namespace
bool is_blank(std::string_view text) {
    const auto decoded = characters(text);
    return std::all_of(decoded.begin(), decoded.end(),
        [](const Character& character) { return whitespace(character.value); });
}
std::string validate_name(std::string_view name) { return validated_name(name); }
void validate_text(std::string_view text, std::size_t max_bytes) {
    if (text.size() > max_bytes || text.find('\0') != std::string_view::npos) {
        throw Error(ErrorCode::invalid, "text exceeds byte budget or contains NUL");
    }
    static_cast<void>(characters(text));
}
nlohmann::json parse_json(std::string_view text, std::size_t max_bytes) {
    if (text.empty() || text.size() > max_bytes) {
        throw Error(ErrorCode::invalid, "JSON exceeds byte budget");
    }
    bound_nesting(text);
    std::vector<std::unordered_set<std::string>> keys;
    const auto callback = [&](int, nlohmann::json::parse_event_t event, nlohmann::json& value) {
        if (event == nlohmann::json::parse_event_t::object_start) keys.emplace_back();
        else if (event == nlohmann::json::parse_event_t::key) {
            if (keys.empty() || !keys.back().insert(value.get<std::string>()).second) {
                throw Error(ErrorCode::invalid, "duplicate JSON key");
            }
        } else if (event == nlohmann::json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    const auto json = nlohmann::json::parse(text, callback, false);
    if (json.is_discarded()) throw Error(ErrorCode::invalid, "malformed JSON");
    return json;
}
Input validate(std::string_view name, std::string_view description, std::string_view document) {
    if (description.size() > 512 || description.find('\0') != std::string_view::npos) {
        throw Error(ErrorCode::invalid, "workspace description exceeds limits");
    }
    static_cast<void>(characters(description));
    return {validated_name(name), std::string(description), validated_document(document)};
}
void validate_id(std::string_view id) {
    if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    })) throw Error(ErrorCode::invalid, "workspace id must contain 32 lowercase hex characters");
}
void validate_revision(std::uint64_t revision) {
    if (revision == 0 || revision >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw Error(ErrorCode::invalid, "workspace expected revision must be a positive int64");
    }
}
} // namespace atx::server::workspace_detail
