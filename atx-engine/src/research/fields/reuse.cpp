#include "atx/engine/research/fields/reuse.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <utility>

#include "atx/core/sha256.hpp"
#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/manifest.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;
using Reason = std::optional<std::string>;

constexpr u64 kPriorManifestLimit = 64ULL << 20;

[[nodiscard]] core::Error refused(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument, "reuse: " + std::move(message));
}

// A string member of a JSON object, empty when absent or not a string.
[[nodiscard]] std::string string_member(const Json &j, const char *key) {
  if (!j.is_object()) {
    return {};
  }
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// An unsigned integer member, nullopt when absent or not one.
[[nodiscard]] std::optional<u64> unsigned_member(const Json &j, const char *key) {
  if (!j.is_object()) {
    return std::nullopt;
  }
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number_unsigned()) {
    return std::nullopt;
  }
  return it->get<u64>();
}

struct Prior {
  std::filesystem::path dir;
  std::string sha256;
  Json manifest;
  std::string seal; // kSealMatch or kSealAbsent
};

[[nodiscard]] core::Status check_role_binding(const Json &manifest, const RoleAxes &role) {
  const auto bound = manifest.find("role");
  if (bound == manifest.end() || !bound->is_object()) {
    return core::Err(refused("the prior fields manifest has no role binding"));
  }
  ATX_TRY(const auto sessions, role.receipt("sessions.i64"));
  ATX_TRY(const auto ids, role.receipt("ids.u64"));
  ATX_TRY(const auto member, role.receipt("member.u8"));
  if (string_member(*bound, "manifest_sha256") != role.manifest_sha256() ||
      string_member(*bound, "sessions_sha256") != sessions.sha256 ||
      string_member(*bound, "ids_sha256") != ids.sha256 ||
      string_member(*bound, "member_sha256") != member.sha256) {
    return core::Err(refused("the prior fields manifest is bound to a different role "
                             "(manifest/sessions/ids/member)"));
  }
  return core::Ok();
}

// kSealMatch, or kSealAbsent for a manifest without a seal block; Err for a seal block whose
// exclusive_end is not this build's seal (a malformed block counts as different).
[[nodiscard]] core::Result<std::string> check_seal(const Json &manifest) {
  const auto seal = manifest.find("seal");
  if (seal == manifest.end()) {
    return core::Ok(std::string(kSealAbsent));
  }
  const std::string recorded = string_member(*seal, "exclusive_end");
  if (recorded != data::kSealBeginDate) {
    return core::Err(refused("the prior fields manifest was built under the seal '" + recorded +
                             "', this build's seal is " + std::string(data::kSealBeginDate) + " (" +
                             std::string(data::kResearchWindowId) + ")"));
  }
  return core::Ok(std::string(kSealMatch));
}

[[nodiscard]] core::Result<Prior> load_prior(const ReuseRequest &request, const RoleAxes &role) {
  Prior prior;
  prior.dir = request.dir;
  ATX_TRY(const auto blob, read_bounded(request.dir / "manifest.json", kPriorManifestLimit));
  ATX_TRY(prior.sha256, core::sha256_hex(std::string_view(blob)));
  if (request.manifest_sha256 && *request.manifest_sha256 != prior.sha256) {
    return core::Err(refused("the prior fields manifest's SHA-256 is not the pinned one"));
  }
  prior.manifest = Json::parse(blob, nullptr, false);
  const Json &m = prior.manifest;
  if (m.is_discarded() || !m.is_object() || string_member(m, "schema") != kFieldsManifestSchema ||
      string_member(m, "status") != "complete") {
    return core::Err(refused("the prior directory is not a complete " +
                             std::string(kFieldsManifestSchema) + " manifest"));
  }
  ATX_TRY_VOID(check_role_binding(m, role));
  ATX_TRY(prior.seal, check_seal(m));
  return core::Ok(std::move(prior));
}

[[nodiscard]] const Json *entry_named(const Json &manifest, const std::string &name) {
  const auto fields = manifest.find("fields");
  if (fields == manifest.end() || !fields->is_array()) {
    return nullptr;
  }
  const auto it = std::find_if(fields->begin(), fields->end(),
                               [&name](const Json &e) { return string_member(e, "name") == name; });
  return it == fields->end() ? nullptr : &*it;
}

[[nodiscard]] const Json *pin_named(const Json &manifest, const std::string &file) {
  const auto files = manifest.find("files");
  if (files == manifest.end() || !files->is_object()) {
    return nullptr;
  }
  const auto pin = files->find(file);
  return pin == files->end() || !pin->is_object() ? nullptr : &*pin;
}

[[nodiscard]] bool layout_matches(const Json &entry, const Json &pin, const FieldPlan &plan,
                                  const RoleAxes &role) {
  const u64 dates = static_cast<u64>(role.dates());
  const u64 instruments = static_cast<u64>(role.instruments());
  const auto shape = entry.find("shape");
  const std::string pinned = string_member(pin, "sha256");
  return string_member(entry, "file") == plan.name + ".f64" &&
         string_member(entry, "dtype") == "<f8" &&
         string_member(entry, "layout") == "date-major" && shape != entry.end() &&
         *shape == Json::array({dates, instruments}) && !pinned.empty() &&
         string_member(entry, "sha256") == pinned &&
         unsigned_member(pin, "bytes") == dates * instruments * 8U;
}

[[nodiscard]] Reason producer_differs(const Json &entry, const ProducerIdentity &current) {
  const auto prior = engine_producer_of(entry);
  if (!prior) {
    return "the prior producer block is refused: " + prior.error().message();
  }
  if (!prior->has_value()) {
    return std::string("the prior entry was not produced by the engine (legacy Python producer)");
  }
  if (!is_known(current)) {
    return std::string("this executable's identity is unknown");
  }
  if (!(**prior == current)) {
    return std::string("the producer identity differs (exe_sha256, git_sha or build_type)");
  }
  return std::nullopt;
}

[[nodiscard]] Reason sources_differ(const Json &entry, const FieldPlan &plan) {
  const auto sources = entry.find("sources");
  if (sources == entry.end() || !sources->is_array() || sources->size() != plan.inputs.size()) {
    return std::string("the prior source list differs");
  }
  for (usize i = 0; i < plan.inputs.size(); ++i) {
    const std::filesystem::path &input = plan.inputs[i];
    const auto digest = digest_file(input);
    if (!digest) {
      return "the source " + input.string() + " cannot be read";
    }
    const Json &recorded = sources->at(i);
    if (string_member(recorded, "sha256") != digest->sha256 ||
        unsigned_member(recorded, "bytes") != digest->bytes) {
      return "the source bytes differ (" + input.filename().string() + ")";
    }
  }
  return std::nullopt;
}

// The first failing rule of reuse.hpp's list, nullopt when the field is reused.
[[nodiscard]] Reason miss_reason(const Json *entry, const Json *pin, const FieldPlan &plan,
                                 const RoleAxes &role, const ProducerIdentity &producer) {
  if (entry == nullptr || pin == nullptr) {
    return std::string("absent from the prior manifest");
  }
  if (!layout_matches(*entry, *pin, plan, role)) {
    return std::string("the prior entry's layout, shape or pin differs");
  }
  if (Reason why = producer_differs(*entry, producer)) {
    return why;
  }
  const auto formula = formula_sha256(*plan.spec);
  if (!formula || string_member(*entry, "formula_sha256") != *formula) {
    return std::string("the formula fingerprint differs");
  }
  return sources_differ(*entry, plan);
}

[[nodiscard]] core::Result<ReusedField> carry(const Prior &prior, const Json &entry,
                                              const Json &pin, const FieldPlan &plan,
                                              const std::filesystem::path &output_dir) {
  const std::string file = plan.name + ".f64";
  ATX_TRY(auto copied, copy_exclusive(prior.dir / file, output_dir / file));
  if (copied.sha256 != string_member(pin, "sha256") ||
      unsigned_member(pin, "bytes") != copied.bytes) {
    return core::Err(refused("the prior payload " + file +
                             " does not match its manifest pin (corrupt prior directory)"));
  }
  ReusedField out;
  out.name = plan.name;
  out.entry = entry;
  out.entry["reused_from"] = Json{{"dir", record_path(prior.dir)},
                                  {"manifest_sha256", prior.sha256},
                                  {"payload_sha256", copied.sha256},
                                  {"mode", "copy"}};
  const auto checks = prior.manifest.find("source_checks");
  if (checks != prior.manifest.end() && checks->is_object() && checks->contains(plan.name)) {
    out.source_checks = checks->at(plan.name);
  }
  out.payload = std::move(copied);
  return core::Ok(std::move(out));
}

} // namespace

const ReusedField *ReuseOutcome::find(std::string_view name) const noexcept {
  const auto it = std::find_if(reused.begin(), reused.end(),
                               [name](const ReusedField &field) { return field.name == name; });
  return it == reused.end() ? nullptr : &*it;
}

core::Result<ReuseOutcome> reuse_payloads(const ReuseRequest &request, const RoleAxes &role,
                                          std::span<const FieldPlan> plans,
                                          const ProducerIdentity &producer,
                                          const std::filesystem::path &output_dir) {
  try {
    ATX_TRY(const auto prior, load_prior(request, role));
    ReuseOutcome out;
    Json reused = Json::array();
    Json recomputed = Json::object();
    for (const FieldPlan &plan : plans) {
      const Json *entry = entry_named(prior.manifest, plan.name);
      const Json *pin = pin_named(prior.manifest, plan.name + ".f64");
      if (Reason why = miss_reason(entry, pin, plan, role, producer)) {
        recomputed[plan.name] = *why;
        continue;
      }
      ATX_TRY(auto field, carry(prior, *entry, *pin, plan, output_dir));
      reused.push_back(plan.name);
      out.reused.push_back(std::move(field));
    }
    out.record = Json{{"dir", record_path(prior.dir)},
                      {"manifest_sha256", prior.sha256},
                      {"seal", prior.seal},
                      {"reused", std::move(reused)},
                      {"recomputed", std::move(recomputed)}};
    return core::Ok(std::move(out));
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::Internal, std::string("reuse: ") + e.what());
  }
}

} // namespace atx::engine::research::fields
