// Pin extraction per holder class (pins.hpp, catalog_detail.hpp).

#include "atx/engine/research/store/catalog/pins.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog {

bool follows_pin(std::string_view pin_kind) noexcept {
  return pin_kind != "field-source" && pin_kind != "spec-digest" && pin_kind != "ledger-head";
}

std::optional<std::string> pin_target(const std::filesystem::path &root, std::string_view text) {
  std::optional<std::string> rel = root_relative(root, text);
  if (!rel) {
    return std::nullopt;
  }
  return path_key(*rel);
}

namespace detail {
namespace {

[[nodiscard]] std::optional<std::string> text_member(const OJson &obj, std::string_view key) {
  const OJson *v = member(obj, key);
  return v != nullptr ? fit_text(*v) : std::nullopt;
}

[[nodiscard]] std::optional<std::string> target_of(const PinScope &s,
                                                   const std::optional<std::string> &text) {
  return text ? pin_target(s.ctx.root, *text) : std::nullopt;
}

void push(const PinScope &s, std::vector<PinRow> &out, std::string pointer, std::string_view kind,
          std::optional<std::string> target, std::string sha) {
  out.push_back(PinRow{s.holder, std::move(pointer), std::string{kind}, std::move(target),
                       std::move(sha)});
}

// {name: {path, sha256}} items under `section` (inputs, locked, change.inputs).
void item_pins(const PinScope &s, const OJson *section, std::initializer_list<std::string_view> at,
               std::string_view kind, std::vector<PinRow> &out) {
  if (section == nullptr || !section->is_object()) {
    return;
  }
  for (auto it = section->begin(); it != section->end(); ++it) {
    const OJson *sha = member(it.value(), "sha256");
    const std::optional<std::string> digest = sha != nullptr ? fit_sha(*sha) : std::nullopt;
    if (!digest) {
      continue;
    }
    std::vector<std::string_view> tokens(at.begin(), at.end());
    tokens.push_back(it.key());
    tokens.push_back("sha256");
    std::string pointer;
    for (const std::string_view token : tokens) {
      pointer += json_pointer({token});
    }
    push(s, out, std::move(pointer), kind, target_of(s, text_member(it.value(), "path")), *digest);
  }
}

// A fields section's manifest pin: <dir or output>/manifest.json.
[[nodiscard]] std::optional<std::string> manifest_in(const PinScope &s, const OJson &fields) {
  std::optional<std::string> dir = text_member(fields, "dir");
  if (!dir) {
    dir = text_member(fields, "output");
  }
  if (!dir) {
    return std::nullopt;
  }
  std::string text = *dir;
  while (!text.empty() && (text.back() == '/' || text.back() == '\\')) {
    text.pop_back();
  }
  return pin_target(s.ctx.root, text + "/manifest.json");
}

// The wave manifest's driver.receipt_digest is "content" (or cannot be read): stage-receipt
// pins are then content digests the catalog does not compute (unresolved).
[[nodiscard]] bool content_digests(const PinScope &s, const OJson &doc) {
  const std::optional<std::string> manifest = text_member(*member(doc, "manifest"), "path");
  const std::optional<std::string> rel = manifest ? root_relative(s.ctx.root, *manifest)
                                                  : std::nullopt;
  if (!rel || has_sealed_year(*rel)) {
    return true;
  }
  const std::filesystem::path file = s.ctx.root / fs_path(*rel);
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size > kMaxOpenBytes) {
    return true;
  }
  std::ifstream in{file, std::ios::binary};
  const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  const std::optional<OJson> wave = parse_strict(text);
  if (!wave) {
    return true;
  }
  const OJson *mode = member_at(*wave, {"driver", "receipt_digest"});
  return mode != nullptr && mode->is_string() && mode->get<std::string>() == "content";
}

} // namespace

void spec_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out) {
  item_pins(s, member(doc, "inputs"), {"inputs"}, "input", out);
  const OJson *fields = member(doc, "fields");
  const OJson *sha = fields != nullptr ? member(*fields, "manifest_sha256") : nullptr;
  if (sha != nullptr && fit_sha(*sha)) {
    push(s, out, json_pointer({"fields", "manifest_sha256"}), "fields-manifest",
         manifest_in(s, *fields), *fit_sha(*sha));
  }
}

void template_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out) {
  item_pins(s, member(doc, "locked"), {"locked"}, "locked", out);
  item_pins(s, member_at(doc, {"change", "inputs"}), {"change", "inputs"}, "input", out);
}

void wave_manifest_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out) {
  constexpr std::string_view kSuffix = "_sha256";
  for (const auto &[section, kind] : {std::pair<std::string_view, std::string_view>{
                                          "fields", "fields-manifest"},
                                      std::pair<std::string_view, std::string_view>{
                                          "rule_cell", "rule-template"}}) {
    const OJson *obj = member(doc, section);
    if (obj == nullptr || !obj->is_object()) {
      continue;
    }
    for (auto it = obj->begin(); it != obj->end(); ++it) {
      const std::string &key = it.key();
      const std::optional<std::string> sha = fit_sha(it.value());
      if (!sha || key.size() <= kSuffix.size() ||
          key.compare(key.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        continue;
      }
      const std::string base = key.substr(0, key.size() - kSuffix.size());
      std::optional<std::string> target =
          (section == "fields" && base == "manifest") ? manifest_in(s, *obj)
                                                      : target_of(s, text_member(*obj, base));
      push(s, out, json_pointer({section, key}), kind, std::move(target), *sha);
    }
  }
}

void receipt_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out) {
  const OJson *bindings = member(doc, "bindings");
  if (bindings != nullptr && bindings->is_array()) {
    for (usize i = 0; i < bindings->size(); ++i) {
      const OJson &b = (*bindings)[i];
      const OJson *sha = member(b, "sha256");
      if (sha == nullptr || !fit_sha(*sha)) {
        continue;
      }
      push(s, out, json_pointer({"bindings", std::to_string(i), "sha256"}), "receipt-binding",
           target_of(s, text_member(b, "path")), *fit_sha(*sha));
    }
  }
  const OJson *exe = member(doc, "executable_sha256");
  if (exe != nullptr && fit_sha(*exe)) {
    const OJson *command = member(doc, "command");
    std::optional<std::string> program;
    if (command != nullptr && command->is_array() && !command->empty()) {
      program = fit_text((*command)[0]);
    }
    push(s, out, json_pointer({"executable_sha256"}), "exe", target_of(s, program),
         *fit_sha(*exe));
  }
  const OJson *logs = member(doc, "logs");
  if (logs != nullptr && logs->is_object()) {
    const std::string run_dir = parent_of(s.holder);
    for (auto it = logs->begin(); it != logs->end(); ++it) {
      const std::optional<std::string> sha = fit_sha(it.value());
      if (!sha) {
        continue;
      }
      push(s, out, json_pointer({"logs", it.key()}), "receipt-log",
           pin_target(s.ctx.root, join_rel(run_dir, it.key())), *sha);
    }
  }
}

void binding_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out) {
  const OJson *sha = member(doc, "spec_sha256");
  if (sha != nullptr && fit_sha(*sha)) {
    push(s, out, json_pointer({"spec_sha256"}), "spec-digest", std::nullopt, *fit_sha(*sha));
  }
}

void verdict_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out) {
  const OJson *spec = member(doc, "spec_sha256");
  if (spec != nullptr && fit_sha(*spec)) {
    push(s, out, json_pointer({"spec_sha256"}), "spec-digest", std::nullopt, *fit_sha(*spec));
  }
  const OJson *head = member_at(doc, {"ledger", "head"});
  if (head != nullptr && fit_sha(*head)) {
    push(s, out, json_pointer({"ledger", "head"}), "ledger-head", std::nullopt, *fit_sha(*head));
  }
}

void wave_result_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out) {
  const OJson *manifest = member(doc, "manifest");
  const OJson *sha = manifest != nullptr ? member(*manifest, "sha256") : nullptr;
  if (sha != nullptr && fit_sha(*sha)) {
    push(s, out, json_pointer({"manifest", "sha256"}), "wave-manifest",
         target_of(s, text_member(*manifest, "path")), *fit_sha(*sha));
  }
  const OJson *receipts = member(doc, "receipts");
  if (receipts == nullptr || !receipts->is_object() || receipts->empty()) {
    return;
  }
  const bool content = manifest == nullptr || content_digests(s, doc);
  const std::string dir = join_rel(parent_of(s.holder), "receipts");
  for (auto it = receipts->begin(); it != receipts->end(); ++it) {
    const std::optional<std::string> digest = fit_sha(it.value());
    if (!digest) {
      continue;
    }
    std::optional<std::string> target =
        content ? std::nullopt : pin_target(s.ctx.root, join_rel(dir, it.key() + ".json"));
    push(s, out, json_pointer({"receipts", it.key()}), "stage-receipt", std::move(target),
         *digest);
  }
}

void fields_manifest_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out) {
  const std::string dir = parent_of(s.holder);
  const OJson *fields = member(doc, "fields");
  if (fields != nullptr && fields->is_array()) {
    for (usize i = 0; i < fields->size(); ++i) {
      const OJson &entry = (*fields)[i];
      const std::string index = std::to_string(i);
      const OJson *sha = member(entry, "sha256");
      const std::optional<std::string> file = text_member(entry, "file");
      if (sha != nullptr && fit_sha(*sha)) {
        push(s, out, json_pointer({"fields", index, "sha256"}), "field-payload",
             file ? pin_target(s.ctx.root, join_rel(dir, *file)) : std::nullopt, *fit_sha(*sha));
      }
      const OJson *sources = member(entry, "sources");
      if (sources == nullptr || !sources->is_array()) {
        continue;
      }
      for (usize j = 0; j < sources->size(); ++j) {
        const OJson &src = (*sources)[j];
        const OJson *src_sha = member(src, "sha256");
        if (src_sha == nullptr || !fit_sha(*src_sha)) {
          continue;
        }
        push(s, out, json_pointer({"fields", index, "sources", std::to_string(j), "sha256"}),
             "field-source", target_of(s, text_member(src, "path")), *fit_sha(*src_sha));
      }
    }
  }
  const OJson *files = member(doc, "files");
  if (files != nullptr && files->is_object()) {
    for (auto it = files->begin(); it != files->end(); ++it) {
      const OJson *sha = member(it.value(), "sha256");
      if (sha == nullptr || !fit_sha(*sha)) {
        continue;
      }
      push(s, out, json_pointer({"files", it.key(), "sha256"}), "field-payload",
           pin_target(s.ctx.root, join_rel(dir, it.key())), *fit_sha(*sha));
    }
  }
  const OJson *registry = member(doc, "registry");
  const OJson *reg_sha = registry != nullptr ? member(*registry, "sha256") : nullptr;
  if (reg_sha != nullptr && fit_sha(*reg_sha)) {
    push(s, out, json_pointer({"registry", "sha256"}), "field-registry",
         target_of(s, text_member(*registry, "path")), *fit_sha(*reg_sha));
  }
}

void emit_pins(const std::string &holder, std::vector<PinRow> pins, IngestResult &out) {
  for (const PinRow &pin : pins) {
    if (pin.target_path && follows_pin(pin.pin_kind)) {
      out.named_files.push_back(*pin.target_path);
    }
    out.pins.push_back(pin);
  }
  out.writes.emplace_back([holder, rows = std::move(pins)](core::db::Database &db) -> core::Status {
    ATX_TRY_VOID(delete_where(db, "pin", "holder_path", holder));
    for (const PinRow &row : rows) {
      ATX_TRY_VOID(insert(db, row));
    }
    return core::Ok();
  });
}

} // namespace detail
} // namespace atx::engine::research::store::catalog
