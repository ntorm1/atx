// Strict JSON reading, Python-compatible JSON writing and the typed-field helpers
// (catalog_detail.hpp).

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog::detail {
namespace {

[[nodiscard]] constexpr bool number_char(char c) noexcept {
  return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

// One number literal (outside strings): an integer must fit i64 (negative) or u64; any other
// literal must convert to a finite double.
[[nodiscard]] bool number_exact(std::string_view token) {
  const bool is_float = token.find_first_of(".eE") != std::string_view::npos;
  const char *first = token.data();
  const char *last = token.data() + token.size();
  if (!is_float) {
    if (!token.empty() && token.front() == '-') {
      i64 value{};
      const auto [ptr, ec] = std::from_chars(first, last, value);
      return ec == std::errc{} && ptr == last;
    }
    u64 value{};
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return ec == std::errc{} && ptr == last;
  }
  f64 value{};
  const auto [ptr, ec] = std::from_chars(first, last, value);
  return ec == std::errc{} && ptr == last && std::isfinite(value);
}

// Every number literal outside strings is exact (see number_exact). Malformed text is left to
// the parser.
[[nodiscard]] bool numbers_exact(std::string_view text) {
  bool in_string = false;
  bool escaped = false;
  usize i = 0;
  // Bounded by the text length: every pass advances i.
  while (i < text.size()) {
    const char c = text[i];
    if (in_string) {
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
      ++i;
      continue;
    }
    if ((c >= '0' && c <= '9') || c == '-') {
      const usize start = i;
      while (i < text.size() && number_char(text[i])) {
        ++i;
      }
      if (!number_exact(text.substr(start, i - start))) {
        return false;
      }
      continue;
    }
    ++i;
  }
  return true;
}

constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

void append_u4(std::string &out, u32 code) {
  out += "\\u";
  out += kHex[(code >> 12U) & 0xFU];
  out += kHex[(code >> 8U) & 0xFU];
  out += kHex[(code >> 4U) & 0xFU];
  out += kHex[code & 0xFU];
}

// The code point at s[i] (well-formed UTF-8: the parser validated it) and its length.
[[nodiscard]] std::pair<u32, usize> decode_utf8(std::string_view s, usize i) {
  const auto b = [&](usize k) { return static_cast<u32>(static_cast<unsigned char>(s[i + k])); };
  const u32 lead = b(0);
  if (lead < 0xE0U && i + 1 < s.size()) {
    return {((lead & 0x1FU) << 6U) | (b(1) & 0x3FU), 2};
  }
  if (lead < 0xF0U && i + 2 < s.size()) {
    return {((lead & 0x0FU) << 12U) | ((b(1) & 0x3FU) << 6U) | (b(2) & 0x3FU), 3};
  }
  if (i + 3 < s.size()) {
    return {((lead & 0x07U) << 18U) | ((b(1) & 0x3FU) << 12U) | ((b(2) & 0x3FU) << 6U) |
                (b(3) & 0x3FU),
            4};
  }
  return {0xFFFDU, 1};
}

// Python's json encoder with ensure_ascii (c_encode_basestring_ascii).
void py_string(std::string_view s, std::string &out) {
  out += '"';
  usize i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80U) {
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
        if (c < 0x20U || c == 0x7FU) {
          append_u4(out, c);
        } else {
          out += static_cast<char>(c);
        }
      }
      ++i;
      continue;
    }
    const auto [code, length] = decode_utf8(s, i);
    if (code >= 0x10000U) {
      const u32 v = code - 0x10000U;
      append_u4(out, 0xD800U + (v >> 10U));
      append_u4(out, 0xDC00U + (v & 0x3FFU));
    } else {
      append_u4(out, code);
    }
    i += length;
  }
  out += '"';
}

[[nodiscard]] core::Status py_value(const OJson &v, bool sort_keys, std::string &out) {
  switch (v.type()) {
  case OJson::value_t::null:
    out += "null";
    return core::Ok();
  case OJson::value_t::boolean:
    out += v.get<bool>() ? "true" : "false";
    return core::Ok();
  case OJson::value_t::number_integer:
    out += std::to_string(v.get<i64>());
    return core::Ok();
  case OJson::value_t::number_unsigned:
    out += std::to_string(v.get<u64>());
    return core::Ok();
  case OJson::value_t::number_float: {
    const f64 d = v.get<f64>();
    if (!std::isfinite(d)) {
      return core::Err(core::ErrorCode::InvalidArgument, "a non-finite float has no JSON form");
    }
    out += py_float_repr(d);
    return core::Ok();
  }
  case OJson::value_t::string:
    py_string(v.get_ref<const std::string &>(), out);
    return core::Ok();
  case OJson::value_t::array: {
    out += '[';
    bool first = true;
    for (const OJson &item : v) {
      out += first ? "" : ",";
      first = false;
      ATX_TRY_VOID(py_value(item, sort_keys, out));
    }
    out += ']';
    return core::Ok();
  }
  case OJson::value_t::object: {
    std::vector<std::pair<const std::string *, const OJson *>> items;
    for (auto it = v.begin(); it != v.end(); ++it) {
      items.emplace_back(&it.key(), &it.value());
    }
    if (sort_keys) {
      std::sort(items.begin(), items.end(),
                [](const auto &a, const auto &b) { return *a.first < *b.first; });
    }
    out += '{';
    bool first = true;
    for (const auto &[key, value] : items) {
      out += first ? "" : ",";
      first = false;
      py_string(*key, out);
      out += ':';
      ATX_TRY_VOID(py_value(*value, sort_keys, out));
    }
    out += '}';
    return core::Ok();
  }
  case OJson::value_t::binary:
  case OJson::value_t::discarded:
    break;
  }
  return core::Err(core::ErrorCode::InvalidArgument, "a value with no JSON form");
}

[[nodiscard]] std::string pointer_token(std::string_view token) {
  std::string out;
  for (const char c : token) {
    if (c == '~') {
      out += "~0";
    } else if (c == '/') {
      out += "~1";
    } else {
      out += c;
    }
  }
  return out;
}

} // namespace

std::optional<OJson> parse_strict(std::string_view text) {
  if (!numbers_exact(text)) {
    return std::nullopt;
  }
  OJson doc = OJson::parse(text.begin(), text.end(), nullptr, false);
  if (doc.is_discarded()) {
    return std::nullopt;
  }
  return doc;
}

std::string compact(const OJson &value) { return value.dump(); }

core::Result<std::string> py_dumps(const OJson &value, bool sort_keys) {
  std::string out;
  ATX_TRY_VOID(py_value(value, sort_keys, out));
  return out;
}

std::string py_float_repr(f64 value) {
  std::array<char, 64> buf{};
  const auto [ptr, ec] =
      std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::scientific);
  if (ec != std::errc{}) {
    return "nan";
  }
  // "[-]d[.ddd]e(+|-)XX": shortest round-trip digits in scientific form.
  std::string_view sci{buf.data(), static_cast<usize>(ptr - buf.data())};
  std::string out;
  if (!sci.empty() && sci.front() == '-') {
    out += '-';
    sci.remove_prefix(1);
  }
  const usize e = sci.find('e');
  std::string digits;
  for (const char c : sci.substr(0, e)) {
    if (c != '.') {
      digits += c;
    }
  }
  i32 exponent = 0;
  if (e != std::string_view::npos) {
    const std::string_view exp_text = sci.substr(e + 1);
    const usize skip = (!exp_text.empty() && exp_text.front() == '+') ? 1 : 0;
    (void)std::from_chars(exp_text.data() + skip, exp_text.data() + exp_text.size(), exponent);
  }
  // Python's repr (format code 'r'): exponent notation when decpt <= -4 or decpt > 16, decpt
  // being the decimal point's position after the first digit's place (value = 0.ddd x 10^decpt).
  const i32 decpt = exponent + 1;
  const auto n = static_cast<i32>(digits.size());
  if (decpt <= -4 || decpt > 16) {
    out += digits.front();
    if (digits.size() > 1) {
      out += '.';
      out += digits.substr(1);
    }
    out += 'e';
    out += exponent < 0 ? '-' : '+';
    const i32 magnitude = exponent < 0 ? -exponent : exponent;
    if (magnitude < 10) {
      out += '0';
    }
    out += std::to_string(magnitude);
    return out;
  }
  if (decpt <= 0) {
    out += "0.";
    out.append(static_cast<usize>(-decpt), '0');
    out += digits;
  } else if (decpt < n) {
    out += digits.substr(0, static_cast<usize>(decpt));
    out += '.';
    out += digits.substr(static_cast<usize>(decpt));
  } else {
    out += digits;
    out.append(static_cast<usize>(decpt - n), '0');
    out += ".0";
  }
  return out;
}

bool is_sha256(std::string_view text) noexcept {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string json_pointer(std::initializer_list<std::string_view> tokens) {
  std::string out;
  for (const std::string_view token : tokens) {
    out += '/';
    out += pointer_token(token);
  }
  return out;
}

std::optional<std::string> schema_of(const OJson &doc) {
  const OJson *s = member(doc, "schema");
  return (s != nullptr && s->is_string()) ? std::optional<std::string>{s->get<std::string>()}
                                          : std::nullopt;
}

std::optional<std::string> fit_text(const OJson &v) {
  return v.is_string() ? std::optional<std::string>{v.get<std::string>()} : std::nullopt;
}

std::optional<std::string> fit_sha(const OJson &v) {
  return (v.is_string() && is_sha256(v.get_ref<const std::string &>()))
             ? std::optional<std::string>{v.get<std::string>()}
             : std::nullopt;
}

std::optional<std::string> fit_relpath(const OJson &v) {
  return (v.is_string() && is_relpath(v.get_ref<const std::string &>()))
             ? std::optional<std::string>{v.get<std::string>()}
             : std::nullopt;
}

std::optional<i64> fit_int(const OJson &v) {
  if (!v.is_number_integer()) {
    return std::nullopt;
  }
  if (v.is_number_unsigned() && v.get<u64>() > static_cast<u64>(std::numeric_limits<i64>::max())) {
    return std::nullopt;
  }
  return v.get<i64>();
}

std::optional<f64> fit_real(const OJson &v) {
  if (!v.is_number_float()) {
    return std::nullopt;
  }
  const f64 d = v.get<f64>();
  if (!std::isfinite(d) || (d == 0.0 && std::signbit(d))) {
    return std::nullopt;
  }
  return d;
}

std::optional<bool> fit_bool(const OJson &v) {
  return v.is_boolean() ? std::optional<bool>{v.get<bool>()} : std::nullopt;
}

const OJson *member(const OJson &obj, std::string_view key) {
  if (!obj.is_object()) {
    return nullptr;
  }
  const auto it = obj.find(std::string{key});
  return it == obj.end() ? nullptr : &*it;
}

const OJson *member_at(const OJson &obj, std::initializer_list<std::string_view> keys) {
  const OJson *at = &obj;
  for (const std::string_view key : keys) {
    at = member(*at, key);
    if (at == nullptr) {
      return nullptr;
    }
  }
  return at;
}

// ---------------------------------------------------------------------------------------------
//  DocFields
// ---------------------------------------------------------------------------------------------

void DocFields::claim(std::string_view key) {
  if (std::find(claimed_.begin(), claimed_.end(), key) == claimed_.end()) {
    claimed_.emplace_back(key);
  }
}

#define ATX_DOC_FIELDS_NULLABLE(Name, Type, Fit)                                                   \
  std::optional<Type> DocFields::Name(std::string_view key) {                                      \
    const OJson *v = member(doc_, key);                                                            \
    if (v == nullptr || v->is_null()) {                                                            \
      claim(key);                                                                                  \
      return std::nullopt;                                                                         \
    }                                                                                              \
    std::optional<Type> fit = Fit(*v);                                                             \
    if (fit) {                                                                                     \
      claim(key);                                                                                  \
    }                                                                                              \
    return fit;                                                                                    \
  }

ATX_DOC_FIELDS_NULLABLE(text, std::string, fit_text)
ATX_DOC_FIELDS_NULLABLE(sha, std::string, fit_sha)
ATX_DOC_FIELDS_NULLABLE(relpath, std::string, fit_relpath)
ATX_DOC_FIELDS_NULLABLE(integer, i64, fit_int)
ATX_DOC_FIELDS_NULLABLE(real, f64, fit_real)

#undef ATX_DOC_FIELDS_NULLABLE

std::optional<std::string> DocFields::json(std::string_view key) {
  claim(key);
  const OJson *v = member(doc_, key);
  if (v == nullptr || v->is_null()) {
    return std::nullopt;
  }
  return compact(*v);
}

#define ATX_DOC_FIELDS_REQUIRED(Name, Type, Fit)                                                   \
  Type DocFields::Name(std::string_view key) {                                                     \
    const OJson *v = member(doc_, key);                                                            \
    if (v != nullptr) {                                                                            \
      std::optional<Type> fit = Fit(*v);                                                           \
      if (fit) {                                                                                   \
        claim(key);                                                                                \
        return std::move(*fit);                                                                    \
      }                                                                                            \
    }                                                                                              \
    shape_ok_ = false;                                                                             \
    return Type{};                                                                                 \
  }

ATX_DOC_FIELDS_REQUIRED(req_text, std::string, fit_text)
ATX_DOC_FIELDS_REQUIRED(req_relpath, std::string, fit_relpath)
ATX_DOC_FIELDS_REQUIRED(req_integer, i64, fit_int)
ATX_DOC_FIELDS_REQUIRED(req_real, f64, fit_real)

#undef ATX_DOC_FIELDS_REQUIRED

std::string DocFields::req_json(std::string_view key) {
  const OJson *v = member(doc_, key);
  if (v == nullptr || v->is_null()) {
    shape_ok_ = false;
    return {};
  }
  claim(key);
  return compact(*v);
}

std::string DocFields::key_order() const {
  OJson keys = OJson::array();
  if (doc_.is_object()) {
    for (auto it = doc_.begin(); it != doc_.end(); ++it) {
      keys.push_back(it.key());
    }
  }
  return compact(keys);
}

std::optional<std::string> DocFields::extra() const {
  OJson out = OJson::object();
  if (doc_.is_object()) {
    for (auto it = doc_.begin(); it != doc_.end(); ++it) {
      if (std::find(claimed_.begin(), claimed_.end(), it.key()) == claimed_.end()) {
        out[it.key()] = it.value();
      }
    }
  }
  if (out.empty()) {
    return std::nullopt;
  }
  return compact(out);
}

} // namespace atx::engine::research::store::catalog::detail
