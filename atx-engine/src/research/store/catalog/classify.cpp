// The class registry and the classifier (classify.hpp).

#include "atx/engine/research/store/catalog/classify.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"

namespace atx::engine::research::store::catalog {
namespace {

using OJson = nlohmann::ordered_json;

constexpr std::array<std::string_view, 3> kFormats{"json", "jsonl", "bytes"};
constexpr std::array<std::string_view, 4> kRenderModes{"", "typed", "doc", "lines"};
constexpr std::array<std::string_view, 12> kIngestFamilies{
    "artifact-only", "run",       "run_start", "stage_receipt", "cycle_binding",  "cycle_verdict",
    "wave_result",   "spec_doc",  "candidate", "trial_line",    "field_manifest", "build_receipt"};

template <usize N>
[[nodiscard]] bool one_of(const std::array<std::string_view, N> &set, std::string_view v) {
  return std::find(set.begin(), set.end(), v) != set.end();
}

[[nodiscard]] std::string text_of(const OJson &obj, std::string_view key) {
  const auto it = obj.find(std::string{key});
  return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

[[nodiscard]] std::vector<std::string> texts_of(const OJson &obj, std::string_view key,
                                                bool lower) {
  std::vector<std::string> out;
  const auto it = obj.find(std::string{key});
  if (it == obj.end() || !it->is_array()) {
    return out;
  }
  for (const OJson &value : *it) {
    if (value.is_string()) {
      out.push_back(lower ? path_key(value.get<std::string>()) : value.get<std::string>());
    }
  }
  return out;
}

[[nodiscard]] core::Result<ClassInfo> class_of(const OJson &row) {
  if (!row.is_object()) {
    return core::Err(core::ErrorCode::InvalidArgument, "classes.json: a class is an object");
  }
  ClassInfo c;
  c.id = text_of(row, "id");
  c.label = text_of(row, "label");
  c.globs = texts_of(row, "globs", true);
  c.schemas = texts_of(row, "schemas", false);
  c.format = text_of(row, "format");
  c.ingest = text_of(row, "ingest");
  c.spec_kind = text_of(row, "spec_kind");
  const auto render = row.find("render");
  if (render != row.end() && render->is_object()) {
    c.render_mode = text_of(*render, "mode");
    c.render_rule = text_of(*render, "rule");
  }
  c.seed_globs = texts_of(row, "seed_globs", false);
  const auto stage = row.find("stage");
  if (stage != row.end() && stage->is_number_integer()) {
    c.stage = static_cast<i32>(stage->get<i64>());
  }
  if (c.id.empty() || !one_of(kFormats, c.format) || !one_of(kIngestFamilies, c.ingest) ||
      !one_of(kRenderModes, c.render_mode)) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "classes.json: class '" + c.id + "' lacks an id or has an unknown format, "
                                                      "ingest family or render mode");
  }
  if (c.ingest == "spec_doc" && c.spec_kind.empty()) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "classes.json: spec_doc class '" + c.id + "' has no spec_kind");
  }
  return c;
}

// One segment against one pattern segment: '*', '?', "[set]" (iterative, with one backtrack
// point for the last '*': linear in practice, bounded by |pattern| * |segment|).
[[nodiscard]] bool match_set(std::string_view pat, usize &p, char c) {
  // pat[p] == '['; on return p is past the closing ']'.
  usize i = p + 1;
  bool negate = false;
  if (i < pat.size() && (pat[i] == '!' || pat[i] == '^')) {
    negate = true;
    ++i;
  }
  bool hit = false;
  bool first = true;
  for (; i < pat.size() && (first || pat[i] != ']'); ++i) {
    first = false;
    if (i + 2 < pat.size() && pat[i + 1] == '-' && pat[i + 2] != ']') {
      hit = hit || (c >= pat[i] && c <= pat[i + 2]);
      i += 2;
    } else {
      hit = hit || c == pat[i];
    }
  }
  p = i < pat.size() ? i + 1 : i;
  return hit != negate;
}

[[nodiscard]] bool match_segment(std::string_view pat, std::string_view s) {
  usize p = 0;
  usize i = 0;
  usize star = std::string_view::npos;
  usize star_i = 0;
  // Each pass advances i or p, or rewinds to the last '*' with star_i advanced: bounded by
  // (|pat| + 1) * (|s| + 1) passes.
  while (i < s.size()) {
    if (p < pat.size() && pat[p] == '*') {
      star = p++;
      star_i = i;
      continue;
    }
    if (p < pat.size() && pat[p] == '?') {
      ++p;
      ++i;
      continue;
    }
    if (p < pat.size() && pat[p] == '[') {
      usize q = p;
      if (match_set(pat, q, s[i])) {
        p = q;
        ++i;
        continue;
      }
    } else if (p < pat.size() && pat[p] == s[i]) {
      ++p;
      ++i;
      continue;
    }
    if (star == std::string_view::npos) {
      return false;
    }
    p = star + 1;
    i = ++star_i;
  }
  while (p < pat.size() && pat[p] == '*') {
    ++p;
  }
  return p == pat.size();
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text) {
  std::vector<std::string_view> out;
  usize start = 0;
  // Bounded by the text length: each pass consumes one segment.
  for (;;) {
    const usize slash = text.find('/', start);
    if (slash == std::string_view::npos) {
      out.push_back(text.substr(start));
      return out;
    }
    out.push_back(text.substr(start, slash - start));
    start = slash + 1;
  }
}

// Depth bounded by the pattern's segment count (one level per pattern segment).
[[nodiscard]] bool match_from(const std::vector<std::string_view> &pat, usize pi,
                              const std::vector<std::string_view> &path, usize si) {
  if (pi == pat.size()) {
    return si == path.size();
  }
  if (pat[pi] == "**") {
    for (usize k = si; k <= path.size(); ++k) {
      if (match_from(pat, pi + 1, path, k)) {
        return true;
      }
    }
    return false;
  }
  return si < path.size() && match_segment(pat[pi], path[si]) &&
         match_from(pat, pi + 1, path, si + 1);
}

[[nodiscard]] bool any_glob(const ClassInfo &c, std::string_view key) {
  return std::any_of(c.globs.begin(), c.globs.end(),
                     [&](const std::string &g) { return glob_match(g, key); });
}

} // namespace

const ClassInfo *ClassRegistry::find(std::string_view id) const noexcept {
  for (const ClassInfo &c : classes) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

core::Result<ClassRegistry> parse_class_registry(std::string_view text) {
  const OJson doc = OJson::parse(text.begin(), text.end(), nullptr, false);
  if (doc.is_discarded() || !doc.is_object() || text_of(doc, "schema") != kClassRegistrySchema) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "classes.json: not a " + std::string{kClassRegistrySchema} + " document");
  }
  const auto rows = doc.find("classes");
  if (rows == doc.end() || !rows->is_array() || rows->empty()) {
    return core::Err(core::ErrorCode::InvalidArgument, "classes.json: no classes");
  }
  ClassRegistry registry;
  for (const OJson &row : *rows) {
    ATX_TRY(ClassInfo c, class_of(row));
    if (registry.find(c.id) != nullptr) {
      return core::Err(core::ErrorCode::InvalidArgument, "classes.json: class '" + c.id +
                                                             "' listed twice");
    }
    registry.classes.push_back(std::move(c));
  }
  const ClassInfo &last = registry.classes.back();
  if (last.id != kOtherClass || last.globs != std::vector<std::string>{"**"}) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "classes.json: the last class must be 'other' with globs [\"**\"]");
  }
  return registry;
}

core::Result<ClassRegistry> load_class_registry(std::string_view path) {
  std::ifstream in{fs_path(path), std::ios::binary};
  if (!in) {
    return core::Err(core::ErrorCode::IoError, "cannot read the class registry " +
                                                   std::string{path});
  }
  const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  return parse_class_registry(text);
}

bool glob_match(std::string_view pattern, std::string_view path) {
  return match_from(split(pattern), 0, split(path), 0);
}

const ClassInfo *classify(const ClassRegistry &registry, std::string_view key,
                          const std::optional<std::string> &schema, bool parsed) {
  for (const ClassInfo &c : registry.classes) {
    if (!any_glob(c, key)) {
      continue;
    }
    if (c.format == "json" && !parsed) {
      continue;
    }
    if (c.format == "json" && !c.schemas.empty() &&
        (!schema || std::find(c.schemas.begin(), c.schemas.end(), *schema) == c.schemas.end())) {
      continue;
    }
    return &c;
  }
  return &registry.classes.back();
}

const ClassInfo *classify_by_path(const ClassRegistry &registry, std::string_view key) {
  for (const ClassInfo &c : registry.classes) {
    if (any_glob(c, key)) {
      return &c;
    }
  }
  return &registry.classes.back();
}

} // namespace atx::engine::research::store::catalog
