#include "json_line.hpp"

#include <array>
#include <utility>

namespace atx::engine::research::ledger::detail {

std::string null_nonfinite_tokens(std::string_view text) {
  static constexpr std::array<std::string_view, 3> kTokens = {"-Infinity", "Infinity", "NaN"};
  std::string out;
  out.reserve(text.size());
  bool in_string = false;
  bool escaped = false;
  usize i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (in_string) {
      out += c;
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      ++i;
      continue;
    }
    if (c == '"') {
      in_string = true;
      out += c;
      ++i;
      continue;
    }
    bool replaced = false;
    for (const std::string_view token : kTokens) {
      if (text.substr(i, token.size()) == token) {
        out += "null";
        i += token.size();
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      out += c;
      ++i;
    }
  }
  return out;
}

core::Result<nlohmann::json> parse_json_line(std::string_view line, const std::string &where) {
  nlohmann::json value =
      nlohmann::json::parse(null_nonfinite_tokens(line), nullptr, /*allow_exceptions=*/false);
  if (value.is_discarded()) {
    return core::Err(core::ErrorCode::ParseError, "trial ledger: " + where + " is not JSON");
  }
  return core::Ok(std::move(value));
}

} // namespace atx::engine::research::ledger::detail
