#include "atx/engine/research/fields/field_spec.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"

namespace atx::engine::research::fields {
namespace {

// Shortest round-trip digits and the decimal point position of a finite |value| (value = 0.d1d2...
// x 10^point), from std::to_chars' shortest scientific form "d[.ddd]e[+-]XX".
struct ShortestDigits {
  std::string digits;
  i64 point{};
};

[[nodiscard]] core::Result<ShortestDigits> shortest_digits(f64 magnitude) {
  std::array<char, 64> buf{};
  const auto printed =
      std::to_chars(buf.data(), buf.data() + buf.size(), magnitude, std::chars_format::scientific);
  if (printed.ec != std::errc{}) {
    return core::Err(core::ErrorCode::Internal, "python_float_repr: to_chars failed");
  }
  const std::string_view text(buf.data(), static_cast<usize>(printed.ptr - buf.data()));
  const auto e = text.find('e');
  if (e == std::string_view::npos) {
    return core::Err(core::ErrorCode::Internal, "python_float_repr: no exponent");
  }
  ShortestDigits out;
  for (const char c : text.substr(0, e)) {
    if (c != '.') {
      out.digits.push_back(c);
    }
  }
  i64 exponent = 0;
  const std::string_view exp_text = text.substr(e + 1);
  const usize skip = (!exp_text.empty() && exp_text.front() == '+') ? 1U : 0U;
  const auto parsed =
      std::from_chars(exp_text.data() + skip, exp_text.data() + exp_text.size(), exponent);
  if (parsed.ec != std::errc{}) {
    return core::Err(core::ErrorCode::Internal, "python_float_repr: bad exponent");
  }
  out.point = exponent + 1;
  return core::Ok(std::move(out));
}

void append_list(std::string &out, const std::vector<std::string> &items, std::string &error) {
  out.push_back('[');
  for (usize i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    const auto s = json_string(items[i]);
    if (!s) {
      error = s.error().message();
      return;
    }
    out += *s;
  }
  out.push_back(']');
}

} // namespace

core::Result<std::string> python_float_repr(f64 value) {
  if (!std::isfinite(value)) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "python_float_repr: non-finite value (allow_nan=False)");
  }
  ATX_TRY(const auto shortest, shortest_digits(std::fabs(value)));
  std::string out = std::signbit(value) ? "-" : "";
  const auto &d = shortest.digits;
  const i64 point = shortest.point;
  const auto count = static_cast<i64>(d.size());
  if (-4 < point && point <= 16) { // Python's 'r' format: fixed notation
    if (point <= 0) {
      out += "0.";
      out.append(static_cast<usize>(-point), '0');
      out += d;
    } else if (point < count) {
      out.append(d, 0, static_cast<usize>(point));
      out.push_back('.');
      out.append(d, static_cast<usize>(point), std::string::npos);
    } else {
      out += d;
      out.append(static_cast<usize>(point - count), '0');
      out += ".0";
    }
    return core::Ok(std::move(out));
  }
  out.push_back(d.front());
  if (d.size() > 1) {
    out.push_back('.');
    out.append(d, 1, std::string::npos);
  }
  const i64 exponent = point - 1;
  out += exponent < 0 ? "e-" : "e+";
  const i64 magnitude = exponent < 0 ? -exponent : exponent;
  if (magnitude < 10) {
    out.push_back('0');
  }
  out += std::to_string(magnitude);
  return core::Ok(std::move(out));
}

core::Result<std::string> json_string(std::string_view text) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x80U) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       "json_string: non-ASCII text is not supported");
    }
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    default:
      if (u < 0x20U || u == 0x7fU) { // Python escapes everything outside ' '..'~'
        out += "\\u00";
        out.push_back(kHex[u >> 4U]);
        out.push_back(kHex[u & 15U]);
      } else {
        out.push_back(c);
      }
      break;
    }
  }
  out.push_back('"');
  return core::Ok(std::move(out));
}

core::Result<std::string> canonical_formula_document(const FieldSpec &spec) {
  const FieldDefinition &d = spec.definition;
  std::string error;
  // Keys in sorted order: clock, definition, domain, non_pit_aspects, point_in_time,
  // source_columns, staleness, units (then the document's definition, field, revision).
  std::string def = "{\"clock\":";
  ATX_TRY(const auto clock, json_string(d.clock));
  def += clock;
  def += ",\"definition\":";
  if (d.definition) {
    ATX_TRY(const auto text, json_string(*d.definition));
    def += text;
  } else {
    def += "null";
  }
  def += ",\"domain\":";
  if (d.domain) {
    ATX_TRY(const auto lo, python_float_repr((*d.domain)[0]));
    ATX_TRY(const auto hi, python_float_repr((*d.domain)[1]));
    def += "[" + lo + "," + hi + "]";
  } else {
    def += "null";
  }
  def += ",\"non_pit_aspects\":";
  append_list(def, d.non_pit_aspects, error);
  def += ",\"point_in_time\":";
  def += d.point_in_time ? "true" : "false";
  def += ",\"source_columns\":";
  append_list(def, d.source_columns, error);
  if (!error.empty()) {
    return core::Err(core::ErrorCode::InvalidArgument, error);
  }
  ATX_TRY(const auto staleness, json_string(d.staleness));
  ATX_TRY(const auto units, json_string(d.units));
  def += ",\"staleness\":" + staleness + ",\"units\":" + units + "}";
  ATX_TRY(const auto name, json_string(spec.name));
  return core::Ok("{\"definition\":" + def + ",\"field\":" + name +
                  ",\"revision\":" + std::to_string(spec.revision) + "}");
}

core::Result<std::string> formula_sha256(const FieldSpec &spec) {
  ATX_TRY(const auto document, canonical_formula_document(spec));
  return core::sha256_hex(std::string_view(document));
}

} // namespace atx::engine::research::fields
