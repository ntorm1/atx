#include "atx/engine/research/fields/field_registry.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <utility>

#include "atx/core/sha256.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/file_io.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr u64 kRegistryLimit = 16ULL << 20;
constexpr std::array<const char *, 12> kRowKeys{
    "name",           "kind",     "builder", "dtype",   "point_in_time", "spec_text",
    "formula_sha256", "requires", "options", "sources", "first_session", "owner"};

[[nodiscard]] core::Error invalid(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument, "field registry: " + std::move(message));
}

[[nodiscard]] bool is_lower_hex64(std::string_view text) noexcept {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// The string member `key` of a row (its presence is checked before).
[[nodiscard]] core::Result<std::string> text_of(const Json &row, const char *key,
                                                const std::string &where, bool non_empty) {
  const Json &value = row.at(key);
  if (!value.is_string() || (non_empty && value.get_ref<const std::string &>().empty())) {
    return core::Err(invalid(where + ": " + key + " must be a " + (non_empty ? "non-empty " : "") +
                             "string"));
  }
  return core::Ok(value.get<std::string>());
}

[[nodiscard]] core::Result<std::vector<std::string>> texts_of(const Json &row, const char *key,
                                                              const std::string &where) {
  const Json &value = row.at(key);
  if (!value.is_array()) {
    return core::Err(invalid(where + ": " + key + " must be an array of strings"));
  }
  std::vector<std::string> out;
  out.reserve(value.size());
  for (const Json &item : value) {
    if (!item.is_string()) {
      return core::Err(invalid(where + ": " + key + " must be an array of strings"));
    }
    out.push_back(item.get<std::string>());
  }
  return core::Ok(std::move(out));
}

[[nodiscard]] core::Status read_enums(const Json &j, const std::string &where, RegistryRow &row) {
  ATX_TRY(const auto kind, text_of(j, "kind", where, true));
  if (kind == field_kind_name(FieldKind::Python)) {
    row.kind = FieldKind::Python;
  } else if (kind == field_kind_name(FieldKind::Engine)) {
    row.kind = FieldKind::Engine;
  } else {
    return core::Err(invalid(where + ": kind must be python or engine, not " + kind));
  }
  ATX_TRY(const auto dtype, text_of(j, "dtype", where, true));
  if (dtype == field_dtype_name(FieldDtype::F64)) {
    row.dtype = FieldDtype::F64;
  } else if (dtype == field_dtype_name(FieldDtype::Group)) {
    row.dtype = FieldDtype::Group;
  } else {
    return core::Err(invalid(where + ": dtype must be f64 or group, not " + dtype));
  }
  return core::Ok();
}

[[nodiscard]] core::Status read_nullables(const Json &j, const std::string &where,
                                          RegistryRow &row) {
  const Json &formula = j.at("formula_sha256");
  if (!formula.is_null()) {
    if (!formula.is_string() || !is_lower_hex64(formula.get_ref<const std::string &>())) {
      return core::Err(invalid(where + ": formula_sha256 must be null or 64 lower-case hex"));
    }
    row.formula_sha256 = formula.get<std::string>();
  }
  const Json &first = j.at("first_session");
  if (!first.is_null()) {
    if (!first.is_string() || !parse_iso_day(first.get_ref<const std::string &>())) {
      return core::Err(invalid(where + ": first_session must be null or a YYYY-MM-DD date"));
    }
    row.first_session = first.get<std::string>();
  }
  return core::Ok();
}

[[nodiscard]] core::Result<RegistryRow> parse_row(const Json &j, usize index) {
  const std::string at = "row " + std::to_string(index);
  if (!j.is_object()) {
    return core::Err(invalid(at + " is not an object"));
  }
  for (const char *key : kRowKeys) {
    if (!j.contains(key)) {
      return core::Err(invalid(at + " has no " + key));
    }
  }
  RegistryRow row;
  ATX_TRY(row.name, text_of(j, "name", at, true));
  const std::string where = at + " (" + row.name + ")";
  ATX_TRY_VOID(read_enums(j, where, row));
  ATX_TRY(row.builder, text_of(j, "builder", where, true));
  const Json &pit = j.at("point_in_time");
  if (!pit.is_boolean()) {
    return core::Err(invalid(where + ": point_in_time must be a boolean"));
  }
  row.point_in_time = pit.get<bool>();
  row.spec_text = j.at("spec_text");
  ATX_TRY_VOID(read_nullables(j, where, row));
  ATX_TRY(row.required_fields, texts_of(j, "requires", where));
  const Json &options = j.at("options");
  if (!options.is_object()) {
    return core::Err(invalid(where + ": options must be an object"));
  }
  row.options = options;
  ATX_TRY(row.sources, texts_of(j, "sources", where));
  ATX_TRY(row.owner, text_of(j, "owner", where, false));
  return core::Ok(std::move(row));
}

[[nodiscard]] Json row_document(const RegistryRow &row) {
  return Json{{"name", row.name},
              {"kind", std::string(field_kind_name(row.kind))},
              {"builder", row.builder},
              {"dtype", std::string(field_dtype_name(row.dtype))},
              {"point_in_time", row.point_in_time},
              {"spec_text", row.spec_text},
              {"formula_sha256", row.formula_sha256 ? Json(*row.formula_sha256) : Json(nullptr)},
              {"requires", Json(row.required_fields)},
              {"options", row.options},
              {"sources", Json(row.sources)},
              {"first_session", row.first_session ? Json(*row.first_session) : Json(nullptr)},
              {"owner", row.owner}};
}

} // namespace

const RegistryRow *FieldRegistry::find(std::string_view name) const noexcept {
  const auto it = std::find_if(rows.begin(), rows.end(),
                               [name](const RegistryRow &row) { return row.name == name; });
  return it == rows.end() ? nullptr : &*it;
}

std::optional<usize> FieldRegistry::index_of(std::string_view name) const noexcept {
  const auto it = std::find_if(rows.begin(), rows.end(),
                               [name](const RegistryRow &row) { return row.name == name; });
  if (it == rows.end()) {
    return std::nullopt;
  }
  return static_cast<usize>(it - rows.begin());
}

std::string_view field_kind_name(FieldKind kind) noexcept {
  switch (kind) {
  case FieldKind::Python:
    return "python";
  case FieldKind::Engine:
    return "engine";
  }
  return "python"; // unreachable: every enumerator is handled above
}

std::string_view field_dtype_name(FieldDtype dtype) noexcept {
  switch (dtype) {
  case FieldDtype::F64:
    return "f64";
  case FieldDtype::Group:
    return "group";
  }
  return "f64"; // unreachable: every enumerator is handled above
}

core::Result<FieldRegistry> parse_field_registry(std::string_view json_text) {
  try {
    const Json j = Json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      return core::Err(invalid("not a JSON object"));
    }
    const auto schema = j.find("schema");
    if (schema == j.end() || !schema->is_string() ||
        schema->get_ref<const std::string &>() != kFieldRegistrySchema) {
      return core::Err(invalid("schema is not atx.field-registry/v1"));
    }
    const auto rows = j.find("fields");
    if (rows == j.end() || !rows->is_array() || rows->empty()) {
      return core::Err(invalid("needs a non-empty fields array"));
    }
    FieldRegistry out;
    out.rows.reserve(rows->size());
    for (usize i = 0; i < rows->size(); ++i) {
      ATX_TRY(auto row, parse_row(rows->at(i), i));
      if (out.find(row.name) != nullptr) {
        return core::Err(invalid("field " + row.name + " is registered twice"));
      }
      out.rows.push_back(std::move(row));
    }
    ATX_TRY(out.sha256, core::sha256_hex(json_text));
    return core::Ok(std::move(out));
  } catch (const std::exception &e) {
    return core::Err(invalid(e.what()));
  }
}

core::Result<FieldRegistry> load_field_registry(const std::filesystem::path &path) {
  ATX_TRY(const auto text, read_bounded(path, kRegistryLimit));
  ATX_TRY(auto registry, parse_field_registry(text));
  registry.path = record_path(path);
  return core::Ok(std::move(registry));
}

Json field_registry_document(const FieldRegistry &registry) {
  Json rows = Json::array();
  for (const RegistryRow &row : registry.rows) {
    rows.push_back(row_document(row));
  }
  return Json{{"schema", std::string(kFieldRegistrySchema)}, {"fields", std::move(rows)}};
}

} // namespace atx::engine::research::fields
