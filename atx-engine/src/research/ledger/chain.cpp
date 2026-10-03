#include "atx/engine/research/ledger/chain.hpp"

#include <utility>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "json_line.hpp"

namespace atx::engine::research::ledger {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::string where(usize physical_line) {
  return "line " + std::to_string(physical_line);
}

// The line's JSON object (ParseError when the line is no object).
[[nodiscard]] core::Result<Json> line_object(std::string_view line, usize physical_line) {
  ATX_TRY(Json value, detail::parse_json_line(line, where(physical_line)));
  if (!value.is_object()) {
    return core::Err(core::ErrorCode::ParseError,
                     "trial ledger: " + where(physical_line) + " is not a JSON object");
  }
  return core::Ok(std::move(value));
}

// One step of the rule: the head after `line` given the head before it.
[[nodiscard]] core::Result<std::string> next_head(const std::string &head, std::string_view line,
                                                  usize physical_line) {
  ATX_TRY(const Json rec, line_object(line, physical_line));
  if (rec.contains("prev_sha256")) {
    return line_sha256(line);
  }
  return fold_head(head, line);
}

} // namespace

std::string_view strip_line_end(std::string_view line) noexcept {
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.remove_suffix(1);
  }
  return line;
}

bool is_blank_line(std::string_view line) noexcept {
  return line.find_first_not_of(" \t\n\r\v\f") == std::string_view::npos;
}

core::Result<std::string> line_sha256(std::string_view line) {
  return core::sha256_hex(strip_line_end(line));
}

core::Result<std::string> fold_head(std::string_view head, std::string_view line) {
  ATX_TRY(const std::string own, line_sha256(line));
  std::string text(head);
  text += own;
  return core::sha256_hex(std::string_view(text));
}

core::Result<bool> is_chained_line(std::string_view line) {
  ATX_TRY(const Json rec, line_object(line, 1U));
  return core::Ok(rec.contains("prev_sha256"));
}

core::Result<ChainHead> chain_head(std::span<const std::string> lines) {
  ChainHead out;
  for (usize k = 0; k < lines.size(); ++k) {
    if (is_blank_line(lines[k])) {
      continue;
    }
    ATX_TRY(out.head_sha256, next_head(out.head_sha256, lines[k], k + 1U));
    ++out.lines;
  }
  return core::Ok(std::move(out));
}

core::Result<std::vector<std::string>> prefix_heads(std::span<const std::string> lines) {
  std::vector<std::string> out;
  out.reserve(lines.size());
  std::string head(kChainGenesis);
  for (usize k = 0; k < lines.size(); ++k) {
    if (!is_blank_line(lines[k])) {
      ATX_TRY(head, next_head(head, lines[k], k + 1U));
    }
    out.push_back(head);
  }
  return core::Ok(std::move(out));
}

core::Result<ChainHead> verify_chain(std::span<const std::string> lines) {
  ChainHead out;
  usize first_chained = 0; // 1-based physical line of the first chained line; 0 = none yet
  for (usize k = 0; k < lines.size(); ++k) {
    if (is_blank_line(lines[k])) {
      continue;
    }
    const usize n = k + 1U;
    ATX_TRY(const Json rec, line_object(lines[k], n));
    const auto schema = rec.find("schema");
    if (schema == rec.end() || !schema->is_string() ||
        schema->get<std::string>() != kLedgerSchema) {
      return core::Err(core::ErrorCode::Internal, "trial ledger " + where(n) + ": schema " +
                                                      (schema == rec.end() ? std::string("None")
                                                                           : schema->dump()) +
                                                      " is not " + std::string(kLedgerSchema));
    }
    const auto link = rec.find("prev_sha256");
    if (link == rec.end() || link->is_null()) {
      if (first_chained != 0U) {
        return core::Err(core::ErrorCode::Internal,
                         "trial ledger " + where(n) + ": an unchained line after the chained " +
                             where(first_chained) +
                             ": every line after the first chained one carries prev_sha256");
      }
      ATX_TRY(out.head_sha256, fold_head(out.head_sha256, lines[k]));
    } else if (!link->is_string() || link->get<std::string>() != out.head_sha256) {
      return core::Err(core::ErrorCode::Internal,
                       "trial ledger " + where(n) + ": hash chain broken: prev_sha256 " +
                           link->dump() + " is not the chain head of the lines before it (" +
                           out.head_sha256.substr(0, 16) + ")");
    } else {
      if (first_chained == 0U) {
        first_chained = n;
      }
      ATX_TRY(out.head_sha256, line_sha256(lines[k]));
    }
    ++out.lines;
  }
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::ledger
