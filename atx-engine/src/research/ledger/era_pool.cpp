#include "atx/engine/research/ledger/era_pool.hpp"

#include <array>
#include <set>
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

core::Status check_era_id(std::string_view id) {
  bool ok = !id.empty();
  for (const char c : id) {
    const bool word = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_';
    ok = ok && word;
  }
  if (!ok) {
    return core::Err(refuse("era id '" + std::string(id) +
                            "' must match [A-Za-z0-9_]+ (it names files)"));
  }
  return core::Ok();
}

core::Status check_era_windows(std::span<const EraWindow> windows, i64 train_begin_ns,
                               i64 train_end_ns, i64 seal_ns) {
  std::set<std::string> seen;
  const EraWindow *prev = nullptr;
  for (const EraWindow &w : windows) {
    ATX_TRY_VOID(check_era_id(w.id));
    if (!seen.insert(w.id).second) {
      return core::Err(refuse("era " + w.id + ": duplicate id"));
    }
    if (!(w.begin_ns < w.end_ns)) {
      return core::Err(refuse("era " + w.id + ": empty window"));
    }
    if (w.end_ns > seal_ns) {
      return core::Err(refuse("era " + w.id +
                              ": window ends after the research seal; refusing (sealed)"));
    }
    if (w.begin_ns < train_begin_ns && train_begin_ns < w.end_ns) {
      return core::Err(refuse("era " + w.id +
                              ": window straddles the TRAIN begin (an era is history before "
                              "TRAIN or lies inside TRAIN)"));
    }
    if (w.begin_ns >= train_begin_ns && w.end_ns > train_end_ns) {
      return core::Err(refuse("era " + w.id + ": a TRAIN era ends after the TRAIN end"));
    }
    if (prev != nullptr) {
      if (w.begin_ns < prev->begin_ns) {
        return core::Err(refuse("era " + w.id + ": eras are not in date order (" + prev->id +
                                " begins later)"));
      }
      if (w.begin_ns < prev->end_ns) {
        return core::Err(refuse("era " + w.id + ": window overlaps era " + prev->id));
      }
    }
    prev = &w;
  }
  if (prev == nullptr) {
    return core::Err(refuse("no era"));
  }
  return core::Ok();
}

core::Result<std::vector<usize>> segment_starts(std::span<const EraSessions> eras) {
  std::vector<usize> starts;
  usize row = 0;
  bool have_last = false;
  i64 last = 0;
  for (const EraSessions &era : eras) {
    if (era.sessions.empty()) {
      return core::Err(refuse("era " + era.id + ": no session"));
    }
    for (usize k = 1; k < era.sessions.size(); ++k) {
      if (era.sessions[k] <= era.sessions[k - 1U]) {
        return core::Err(refuse("era " + era.id + ": sessions are not strictly increasing"));
      }
    }
    if (have_last && era.sessions.front() <= last) {
      return core::Err(refuse("era " + era.id +
                              ": its first session is not after the previous era's last "
                              "(overlap or order)"));
    }
    starts.push_back(row);
    row += era.sessions.size();
    last = era.sessions.back();
    have_last = true;
  }
  if (starts.empty()) {
    return core::Err(refuse("no era"));
  }
  return core::Ok(std::move(starts));
}

core::Result<f64> weighted_mean(std::span<const f64> values, std::span<const f64> weights) {
  if (values.empty() || values.size() != weights.size()) {
    return core::Err(refuse("weighted_mean: one weight per value"));
  }
  if (values.size() == 1U) {
    return core::Ok(values[0]);
  }
  f64 total = 0.0;
  bool negative = false;
  for (const f64 w : weights) {
    total += w;
    negative = negative || w < 0.0;
  }
  if (!(total > 0.0) || negative) {
    return core::Err(refuse("weighted_mean: weights must be >= 0 with a positive sum"));
  }
  f64 acc = 0.0;
  for (usize i = 0; i < values.size(); ++i) {
    acc += values[i] * weights[i];
  }
  return core::Ok(acc / total);
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
