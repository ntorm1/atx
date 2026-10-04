#pragma once

// Test support of the field registry, builder-kind and manifest tests (atx-engine-research-fields-
// tests): K-P9-1 rows in the shape the Python registry writes them (lane A1: options carry the
// producer group), registries, and build specs on the identity fixture.

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/build_spec.hpp"
#include "atx/engine/research/fields/field_registry.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/finra_asof_field.hpp"
#include "atx/engine/research/fields/volume_mean_field.hpp"
#include "research/research_fields_test_support.hpp"

namespace atx::engine::research::fields::test {

inline const fs::path &fixture_dir() {
  static const fs::path dir{ATX_RESEARCH_FIELDS_FIXTURE};
  return dir;
}

// The declared spec of a ported field (vol_126, si_shares or si_dtc).
inline const FieldSpec &ported_spec(const std::string &name) {
  return name == "vol_126" ? vol_126_spec() : *finra_spec(name);
}

// The K-P9-1 row of a ported field. An engine row's builder is its BuilderKind id (ruling P5); a
// python row's builder is the Python module.
inline nlohmann::json ported_row(const std::string &name, const std::string &kind = "engine") {
  const FieldSpec &s = ported_spec(name);
  return nlohmann::json{
      {"name", name},
      {"kind", kind},
      {"builder", kind == "engine" ? name : std::string("prepare_research_fields")},
      {"dtype", "f64"},
      {"point_in_time", true},
      {"spec_text", nlohmann::json{{"units", s.definition.units}, {"clock", s.definition.clock}}},
      {"formula_sha256", formula_sha256(s).value()},
      {"requires", nlohmann::json::array()},
      {"options", nlohmann::json{{"group", s.group}}},
      {"sources", nlohmann::json::array({name == "vol_126" ? "role" : "finra"})},
      {"first_session", nullptr},
      {"owner", name == "vol_126" ? "FIELD_MODULES:research_fields_price" : "builder:FIELDS"}};
}

// A python row of a field the engine does not build.
inline nlohmann::json python_row(const std::string &name) {
  return nlohmann::json{{"name", name},
                        {"kind", "python"},
                        {"builder", "prepare_research_fields"},
                        {"dtype", "f64"},
                        {"point_in_time", true},
                        {"spec_text", nlohmann::json::object()},
                        {"formula_sha256", std::string(64, 'f')},
                        {"requires", nlohmann::json::array()},
                        {"options", nlohmann::json{{"group", "issuer"}}},
                        {"sources", nlohmann::json::array({"sec"})},
                        {"first_session", nullptr},
                        {"owner", "builder:ISSUER_FIELDS"}};
}

// The registry document of `rows`, with one informational top-level key.
inline nlohmann::json registry_document(const std::vector<nlohmann::json> &rows) {
  return nlohmann::json{{"schema", "atx.field-registry/v1"},
                        {"generated_from", "atx-engine-research-fields-tests"},
                        {"fields", nlohmann::json(rows)}};
}

inline FieldRegistry registry_of(const std::vector<nlohmann::json> &rows) {
  return parse_field_registry(registry_document(rows).dump()).value();
}

// The three ported fields as engine rows, in the Python manifest order.
inline FieldRegistry engine_registry() {
  return registry_of({ported_row("si_shares"), ported_row("si_dtc"), ported_row("vol_126")});
}

inline std::string fixture_role_sha256() {
  return sha256_of(read_bytes(fixture_dir() / "role" / "manifest.json"));
}

// A build of `names` on the fixture role and FINRA inputs into `out`.
inline BuildSpec fixture_spec(const fs::path &out, std::vector<std::string> names) {
  BuildSpec spec;
  spec.role_dir = fixture_dir() / "role";
  spec.role_manifest_sha256 = fixture_role_sha256();
  spec.output_dir = out;
  spec.fields = std::move(names);
  spec.finra = fixture_dir() / "finra";
  return spec;
}

} // namespace atx::engine::research::fields::test
