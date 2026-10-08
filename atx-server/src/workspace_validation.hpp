#pragma once

#include <cstdint>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace atx::server::workspace_detail {
struct Input { std::string name, description, document; };
[[nodiscard]] Input validate(std::string_view name, std::string_view description,
                             std::string_view document);
void validate_id(std::string_view id);
void validate_revision(std::uint64_t revision);
[[nodiscard]] std::string validate_name(std::string_view name);
void validate_text(std::string_view text, std::size_t max_bytes);
[[nodiscard]] bool is_blank(std::string_view text);
[[nodiscard]] nlohmann::json parse_json(std::string_view text, std::size_t max_bytes);
} // namespace atx::server::workspace_detail
