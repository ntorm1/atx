#pragma once

// atx::engine::research::store::catalog -- the class registry and the file classifier (P9 SQL2;
// sql-design sections 1 and 3.9).
//
// The registry is data: atx-engine/schemas/research_store/classes.json, read at run time (a
// registry of classes, not a schema generator; ruling SQL-5). One row per artifact class of
// sql-design section 1: id, label (I1..P1), path globs, the JSON schema ids it carries, file
// format (json | jsonl | bytes), ingest table family or "artifact-only", spec_doc kind,
// render mode / rule (or none), writer, pinned_by, stage, and the specific globs (if any) that
// seed every catalog walk. `legacy_allow` (dated) and `domains` list the schema literals that
// name no class (the guard pytest test_research_store_classes.py reads those; the catalog
// ignores them).
//
// Classification of a root-relative path: the first class, in registry order, one of whose
// globs matches the path_key and -- for a json class that lists schemas -- whose schemas hold
// the document's top-level "schema" string. The registry's last class `other` (glob "**")
// takes the rest. Globs: '/'-separated segments; "**" any number of segments; '*' any run and
// '?' one character within a segment; "[a-z0-9]" a character set ("[!...]" negated). Globs are
// lower-case and match the path_key.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::store::catalog {

inline constexpr std::string_view kClassRegistrySchema = "atx.research-store-classes/v1";
inline constexpr std::string_view kOtherClass = "other";
inline constexpr std::string_view kArtifactOnly = "artifact-only";

struct ClassInfo {
  std::string id;
  std::string label;
  std::vector<std::string> globs;
  std::vector<std::string> schemas;
  std::string format;      // json | jsonl | bytes
  std::string ingest;      // a table family (run, run_start, ...) or "artifact-only"
  std::string spec_kind;   // spec_doc.kind for the spec_doc family, else ""
  std::string render_mode; // typed | doc | lines | "" (no render rule)
  std::string render_rule; // py-indent2 | py-indent2-sorted | py-indent2-noascii |
                           // py-compact-sorted-lines | ""
  std::vector<std::string> seed_globs; // root-relative globs every catalog walk starts from
  i32 stage{1};

  [[nodiscard]] bool artifact_only() const noexcept { return ingest == kArtifactOnly; }
};

struct ClassRegistry {
  std::vector<ClassInfo> classes; // registry order; the last is `other`

  [[nodiscard]] const ClassInfo *find(std::string_view id) const noexcept;
};

// Parse the registry document. Err(InvalidArgument) for a document that is not
// kClassRegistrySchema, a class without id / format / ingest, a duplicate id, an unknown
// format, render mode or ingest family, or a registry whose last class is not `other`.
[[nodiscard]] core::Result<ClassRegistry> parse_class_registry(std::string_view text);

// Read and parse the registry file. Err(IoError) when it cannot be read.
[[nodiscard]] core::Result<ClassRegistry> load_class_registry(std::string_view path);

// Glob match of a '/'-separated pattern against a '/'-separated path (see header comment).
[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view path);

// The class of `key` (a path_key). `schema` is the document's top-level "schema" string, if
// the file parsed as a JSON object holding one; `parsed` is false for a json file that did
// not parse (it is then `other`). Never null for a registry parse_class_registry accepted.
[[nodiscard]] const ClassInfo *classify(const ClassRegistry &registry, std::string_view key,
                                        const std::optional<std::string> &schema, bool parsed);

// The class a path would take before its bytes are read: json / jsonl / bytes format by its
// globs alone (the first glob-matching class in registry order).
[[nodiscard]] const ClassInfo *classify_by_path(const ClassRegistry &registry,
                                                std::string_view key);

} // namespace atx::engine::research::store::catalog
