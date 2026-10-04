#pragma once

// atx::engine::research::fields -- the field registry (contract K-P9-1, written by the Python side
// as atx-engine/tools/field_registry.json): one row per research field, in registration order,
// which is the manifest order.
//
//   {"schema": "atx.field-registry/v1", "fields": [row, ...], <informational keys>}
//   row: {name, kind: "python" | "engine", builder, dtype: "f64" | "group", point_in_time,
//         spec_text, formula_sha256, requires[], options{}, sources[], first_session, owner}
//
// Every row carries all twelve keys (a missing one is refused); row keys outside the twelve and
// top-level keys other than schema / fields are ignored (informational). Names are distinct. For an
// engine row `builder` is a BuilderKind id (registry.hpp); for a python row it names the Python
// module and the engine never reads it. spec_text is kept as opaque JSON. formula_sha256 is null
// or 64 lower-case hex digits; first_session is null or a strict YYYY-MM-DD date.
//
// The JSON types are part of this interface: a row's spec_text and options are JSON documents, and
// every includer parses JSON. Cold path; nothing here throws.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

inline constexpr std::string_view kFieldRegistrySchema = "atx.field-registry/v1";

enum class FieldKind : u8 { Python, Engine };
enum class FieldDtype : u8 { F64, Group };

struct RegistryRow {
  std::string name;
  FieldKind kind{FieldKind::Python};
  std::string builder;
  FieldDtype dtype{FieldDtype::F64};
  bool point_in_time{true};
  nlohmann::json spec_text;
  std::optional<std::string> formula_sha256;
  std::vector<std::string> required_fields; // the row's "requires" (a C++20 keyword)
  nlohmann::json options = nlohmann::json::object();
  std::vector<std::string> sources;
  std::optional<std::string> first_session;
  std::string owner;
};

struct FieldRegistry {
  std::vector<RegistryRow> rows; // registration order
  std::string path;              // record_path of the file read; empty for parsed text
  std::string sha256;            // SHA-256 of the bytes parsed

  // The row named `name`, nullptr when the registry has none (the pointer lives as long as rows).
  [[nodiscard]] const RegistryRow *find(std::string_view name) const noexcept;
  // The registration index of `name`, nullopt when absent.
  [[nodiscard]] std::optional<usize> index_of(std::string_view name) const noexcept;
};

[[nodiscard]] std::string_view field_kind_name(FieldKind kind) noexcept;
[[nodiscard]] std::string_view field_dtype_name(FieldDtype dtype) noexcept;

// Err(InvalidArgument) on malformed text, another schema, an empty or malformed row list, a row
// missing a key or holding a value outside the contract above, or a repeated name.
[[nodiscard]] core::Result<FieldRegistry> parse_field_registry(std::string_view json_text);

// parse_field_registry of the file's bytes (at most 16 MiB), with path and sha256 set.
// Err(IoError) when it cannot be read.
[[nodiscard]] core::Result<FieldRegistry> load_field_registry(const std::filesystem::path &path);

// The registry document {"schema", "fields": [every row with its twelve keys]}: parsing its dump
// gives the same rows back (informational top-level keys are not carried).
[[nodiscard]] nlohmann::json field_registry_document(const FieldRegistry &registry);

} // namespace atx::engine::research::fields
