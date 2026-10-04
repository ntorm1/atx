#pragma once

// atx::engine::research::fields -- the declared definition of a field and the formula part of its
// reuse fingerprint.
//
// A field's formula fingerprint is the SHA-256 of the canonical JSON document
//   {"definition": {"clock", "definition", "domain", "non_pit_aspects", "point_in_time",
//                   "source_columns", "staleness", "units"},
//    "field": <name>, "revision": <revision>}
// written with sorted keys, "," and ":" separators, no whitespace, ASCII escapes and Python float
// repr, i.e. exactly prepare_research_fields.py formula_id(name, spec_definition(name, lag)).
// Equal spec text gives an equal fingerprint on both sides, so the engine path reuses a payload the
// Python built and the reverse (the manifest entry's formula_sha256). The text itself is copied
// verbatim from the Python spec until one field registry file serves both (migration plan slice
// 3); the fingerprint test pins each copy against the Python's value.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

struct FieldDefinition {
  std::string units;
  std::string clock;
  std::string staleness;
  std::vector<std::string> source_columns;
  std::optional<std::string> definition;
  bool point_in_time{true};
  std::vector<std::string> non_pit_aspects;
  std::optional<std::array<f64, 2>> domain;
};

struct FieldSpec {
  std::string name;
  std::string group;
  // The declared formula name (e.g. price-vol-126-lag1-v1); empty when none is declared.
  std::string formula_id;
  i64 revision{1};
  FieldDefinition definition;
  std::vector<std::string> caveats;
  std::string min_history; // empty when none is declared
};

// Python repr(float): the shortest round-trip digits, fixed notation when the decimal exponent lies
// in [-4, 16), else d[.ddd]e+XX. Err for a non-finite value (the manifests are written with
// allow_nan=False).
[[nodiscard]] core::Result<std::string> python_float_repr(f64 value);

// A JSON string literal as Python json.dumps(ensure_ascii=True) writes it. Err on a byte outside
// ASCII (the field specs are ASCII; a UTF-16 escape of other text is not needed and not guessed).
[[nodiscard]] core::Result<std::string> json_string(std::string_view text);

// The canonical document above, and its SHA-256 (lower-case hex).
[[nodiscard]] core::Result<std::string> canonical_formula_document(const FieldSpec &spec);
[[nodiscard]] core::Result<std::string> formula_sha256(const FieldSpec &spec);

} // namespace atx::engine::research::fields
