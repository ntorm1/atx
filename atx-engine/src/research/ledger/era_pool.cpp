#include "atx/engine/research/ledger/era_pool.hpp"

#include <array>
#include <utility>

#include "atx/core/sha256.hpp"

namespace atx::engine::research::ledger {
namespace {

[[nodiscard]] core::Error refuse(const std::string &message) {
  return core::Error(core::ErrorCode::InvalidArgument, "era pool: " + message);
}

[[nodiscard]] std::string zero_pad(i64 value, usize width) {
  std::string digits = std::to_string(value < 0 ? -value : value);
  while (digits.size() < width) {
    digits.insert(digits.begin(), '0');
  }
  return value < 0 ? "-" + digits : digits;
}

// A compact JSON array of ASCII strings: json.dumps(list, separators=(",", ":")).
[[nodiscard]] core::Result<std::string> json_string_array(std::span<const std::string> items) {
  std::string out = "[";
  for (usize i = 0; i < items.size(); ++i) {
    if (i != 0U) {
      out += ',';
    }
    ATX_TRY(const std::string quoted, python_json_string(items[i]));
    out += quoted;
  }
  out += ']';
  return core::Ok(std::move(out));
}

} // namespace

core::Result<std::string> python_json_string(std::string_view text) {
  static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string out = "\"";
  for (const char c : text) {
    const unsigned byte = static_cast<unsigned char>(c);
    if (byte >= 0x80U) {
      return core::Err(refuse("a non-ASCII identity text is refused"));
    }
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    default:
      if (byte < 0x20U || byte == 0x7FU) {
        out += "\\u00";
        out += kHex[byte >> 4U];
        out += kHex[byte & 0x0FU];
      } else {
        out += c;
      }
    }
  }
  out += '"';
  return core::Ok(std::move(out));
}

std::string iso_date_from_days(i64 days) {
  // H. Hinnant's civil_from_days (proleptic Gregorian).
  const i64 z = days + 719468;
  const i64 era = (z >= 0 ? z : z - 146096) / 146097;
  const i64 doe = z - era * 146097;
  const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const i64 mp = (5 * doy + 2) / 153;
  const i64 day = doy - (153 * mp + 2) / 5 + 1;
  const i64 month = mp < 10 ? mp + 3 : mp - 9;
  const i64 year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  return zero_pad(year, 4U) + "-" + zero_pad(month, 2U) + "-" + zero_pad(day, 2U);
}

core::Result<std::string> pooled_sha256(std::span<const std::string> parts) {
  if (parts.empty()) {
    return core::Err(refuse("pooled_sha256: no part"));
  }
  if (parts.size() == 1U) {
    return core::Ok(parts[0]);
  }
  ATX_TRY(const std::string list, json_string_array(parts));
  ATX_TRY(const std::string schema, python_json_string(kEraPoolSchema));
  const std::string doc = "{\"parts\":" + list + ",\"schema\":" + schema + "}";
  return core::sha256_hex(std::string_view(doc));
}

std::string pool_label(std::span<const std::string> dirs) {
  std::string out(kPoolPrefix);
  for (usize i = 0; i < dirs.size(); ++i) {
    if (i != 0U) {
      out += ',';
    }
    for (const char c : dirs[i]) {
      out += c == '\\' ? '/' : c;
    }
  }
  return out;
}

core::Result<std::string> trial_id(std::string_view kind, std::string_view series_sha256) {
  const std::array<std::string, 2> items = {std::string(kind), std::string(series_sha256)};
  ATX_TRY(const std::string ident, json_string_array(items));
  ATX_TRY(const std::string digest, core::sha256_hex(std::string_view(ident)));
  return core::Ok(digest.substr(0, 16));
}

core::Result<std::string> pooled_trial_id(std::string_view kind,
                                          std::span<const std::string> parts) {
  ATX_TRY(const std::string pooled, pooled_sha256(parts));
  return trial_id(kind, pooled);
}

} // namespace atx::engine::research::ledger
