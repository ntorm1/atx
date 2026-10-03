#include "atx/engine/research/fields/research_fields_cli.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/field_registry.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_stats.hpp"
#include "atx/engine/research/fields/file_io.hpp"
#include "atx/engine/research/fields/manifest.hpp"
#include "atx/engine/research/fields/producer.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/reuse.hpp"
#include "atx/engine/research/fields/role_axes.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr u64 kSpecLimit = 1ULL << 20;

[[nodiscard]] core::Error invalid(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "atx-research-fields spec: " + std::move(message));
}

[[nodiscard]] Json number_or_null(const std::optional<f64> &value) {
  return value ? Json(*value) : Json(nullptr);
}

[[nodiscard]] Json cells(u64 member, u64 finite) {
  return Json{{"member_cells", member},
              {"finite_member_cells", finite},
              {"finite_member_frac", number_or_null(rounded_fraction(finite, member))}};
}

// FieldWriter.coverage() of the Python builder, with digest_and_quantiles' member quantiles.
[[nodiscard]] Json coverage_json(const Coverage &c) {
  Json out = cells(c.member_cells, c.finite_member_cells);
  out["score_window"] = cells(c.score_member_cells, c.score_finite_member_cells);
  Json years = Json::object();
  for (const auto &y : c.per_year) {
    years[std::to_string(y.year)] = cells(y.member_cells, y.finite_member_cells);
  }
  out["per_year"] = std::move(years);
  out["finite_cells_all"] = c.finite_cells_all;
  out["member_finite_min"] = number_or_null(c.member_finite_min);
  out["member_finite_max"] = number_or_null(c.member_finite_max);
  out["member_finite_mean"] = number_or_null(c.member_finite_mean);
  if (c.member_finite_quantiles) {
    const auto &q = *c.member_finite_quantiles;
    out["member_finite_quantiles"] =
        Json{{"p0.1", q.p0_1}, {"p1", q.p1}, {"p50", q.p50}, {"p99", q.p99}, {"p99.9", q.p99_9}};
  } else {
    out["member_finite_quantiles"] = nullptr;
  }
  return out;
}

[[nodiscard]] Json sources_json(const std::vector<SourceRecord> &sources) {
  Json out = Json::array();
  for (const auto &s : sources) {
    out.push_back(Json{{"path", s.path}, {"bytes", s.bytes}, {"sha256", s.sha256}});
  }
  return out;
}

// A kind's blocks into its entry: an object member merges one level deep, anything else is set.
void merge_blocks(Json &entry, const Json &blocks) {
  for (auto it = blocks.begin(); it != blocks.end(); ++it) {
    Json &slot = entry[it.key()];
    if (slot.is_object() && it->is_object()) {
      for (auto sub = it->begin(); sub != it->end(); ++sub) {
        slot[sub.key()] = sub.value();
      }
    } else {
      slot = it.value();
    }
  }
}

[[nodiscard]] core::Result<Json> entry_json(const FieldPlan &plan, const KindOutput &built) {
  ATX_TRY(const auto formula, formula_sha256(*plan.spec));
  Json entry{{"name", built.field.name},
             {"file", built.field.name + ".f64"},
             {"bytes", built.field.bytes},
             {"sha256", built.field.sha256},
             {"formula_sha256", formula},
             {"sources", sources_json(built.sources)},
             {"coverage", coverage_json(built.field.coverage)}};
  merge_blocks(entry, built.blocks);
  return core::Ok(std::move(entry));
}

// Builds every plan through its kind, in order, sharing one context; the receipt entries.
[[nodiscard]] core::Result<Json> build_entries(const BuildSpec &spec, const RoleAxes &role,
                                               std::span<const FieldPlan> plans) {
  BuildContext ctx{spec, role, {}};
  Json fields = Json::array();
  for (const FieldPlan &plan : plans) {
    ATX_TRY(const auto built, plan.kind->build(plan, ctx));
    ATX_TRY(Json entry, entry_json(plan, built));
    fields.push_back(std::move(entry));
  }
  return core::Ok(std::move(fields));
}

[[nodiscard]] core::Status require_output_dir(const BuildSpec &spec) {
  std::error_code ec;
  if (!std::filesystem::is_directory(spec.output_dir, ec)) {
    return core::Err(invalid("output_dir is not a directory: " + spec.output_dir.string()));
  }
  return core::Ok();
}

// The role is re-pinned after the last field: its axes must not have moved under the build.
[[nodiscard]] core::Status repin_role(const BuildSpec &spec) {
  const auto repinned = RoleAxes::load(spec.role_dir, spec.role_manifest_sha256);
  if (!repinned) {
    return core::Err(repinned.error());
  }
  return core::Ok();
}

[[nodiscard]] Json receipt_json(const BuildSpec &spec, std::string_view spec_sha256,
                                const RoleAxes &role, Json fields,
                                const ProducerIdentity &producer) {
  return Json{{"schema", kReceiptSchema},
              {"status", "complete"},
              {"engine", Json{{"name", kEngineName},
                              {"fields", Json(spec.fields)},
                              {"exe_sha256", producer.exe_sha256},
                              {"git_sha", producer.git_sha},
                              {"build_type", producer.build_type}}},
              {"spec_sha256", std::string(spec_sha256)},
              {"role", Json{{"dir", record_path(spec.role_dir)},
                            {"manifest_sha256", role.manifest_sha256()}}},
              {"output_dir", record_path(spec.output_dir)},
              {"research_window", std::string(data::kResearchWindowId)},
              {"seal", std::string(data::kSealBeginDate)},
              {"fields", std::move(fields)}};
}

[[nodiscard]] core::Result<std::string> required_string(const Json &j, const char *key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string() || it->get<std::string>().empty()) {
    return core::Err(invalid(std::string("needs a non-empty string ") + key));
  }
  return core::Ok(it->get<std::string>());
}

[[nodiscard]] bool is_known_key(std::string_view key, bool registry) {
  return key == "schema" || key == "role" || key == "output_dir" || key == "fields" ||
         key == "finra" || (registry && key == "reuse");
}

// "reuse": {"dir", "manifest_sha256"?} of a v2 spec; the pin is compared lower-case.
[[nodiscard]] core::Result<ReuseRequest> parse_reuse(const Json &j) {
  if (!j.is_object()) {
    return core::Err(invalid("reuse must be an object {dir, manifest_sha256}"));
  }
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (it.key() != "dir" && it.key() != "manifest_sha256") {
      return core::Err(invalid("unknown reuse key " + it.key()));
    }
  }
  ReuseRequest out;
  ATX_TRY(const auto dir, required_string(j, "dir"));
  out.dir = std::filesystem::path(dir);
  if (j.contains("manifest_sha256")) {
    ATX_TRY(auto pin, required_string(j, "manifest_sha256"));
    std::transform(pin.begin(), pin.end(), pin.begin(), [](char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    const bool hex = pin.size() == 64 && std::all_of(pin.begin(), pin.end(), [](char c) {
                       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                     });
    if (!hex) {
      return core::Err(invalid("reuse.manifest_sha256 must be 64 hex digits"));
    }
    out.manifest_sha256 = std::move(pin);
  }
  return core::Ok(std::move(out));
}

[[nodiscard]] core::Status parse_fields(const Json &j, BuildSpec &out) {
  const auto fields = j.find("fields");
  if (fields == j.end() || !fields->is_array() || fields->empty()) {
    return core::Err(invalid("needs a non-empty fields array"));
  }
  for (const auto &f : *fields) {
    if (!f.is_string()) {
      return core::Err(invalid("fields holds a non-string"));
    }
    std::string name = f.get<std::string>();
    if (std::find(out.fields.begin(), out.fields.end(), name) != out.fields.end()) {
      return core::Err(invalid("field " + name + " named twice"));
    }
    out.fields.push_back(std::move(name));
  }
  return core::Ok();
}

[[nodiscard]] std::string usage() {
  return "usage: atx-research-fields build --spec SPEC.json --receipt RECEIPT.json"
         " [--registry REGISTRY.json]\n";
}

struct Args {
  std::optional<std::string_view> spec;
  std::optional<std::string_view> receipt;
  std::optional<std::string_view> registry;
};

// Exit 0 when parsed, else 2 (usage written to `err`).
[[nodiscard]] int parse_args(std::span<const std::string_view> args, Args &out,
                             std::ostream &err) {
  if (args.size() < 2 || args[1] != "build") {
    err << usage();
    return 2;
  }
  for (usize i = 2; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    std::optional<std::string_view> *slot = arg == "--spec"       ? &out.spec
                                            : arg == "--receipt"  ? &out.receipt
                                            : arg == "--registry" ? &out.registry
                                                                  : nullptr;
    if (slot == nullptr || i + 1 >= args.size()) {
      err << "atx-research-fields: unknown argument " << arg << "\n" << usage();
      return 2;
    }
    *slot = args[i + 1];
    ++i;
  }
  if (!out.spec || !out.receipt) {
    err << usage();
    return 2;
  }
  return 0;
}

[[nodiscard]] int run_plain(const BuildSpec &spec, std::string_view digest,
                            const ProducerIdentity &producer, std::string_view receipt_path,
                            std::ostream &out, std::ostream &err) {
  const auto receipt = build_fields(spec, digest, producer);
  if (!receipt) {
    err << receipt.error().message() << "\n";
    return 1;
  }
  const auto written = write_exclusive(std::filesystem::path(receipt_path), *receipt);
  if (!written) {
    err << written.error().message() << "\n";
    return 1;
  }
  out << "atx-research-fields: built " << spec.fields.size() << " field(s); receipt "
      << receipt_path << "\n";
  return 0;
}

[[nodiscard]] int run_registry(const BuildSpec &spec, const FieldRegistry &registry,
                               std::string_view digest, const ProducerIdentity &producer,
                               std::string_view receipt_path, std::ostream &out,
                               std::ostream &err) {
  const auto built = build_registry_fields(spec, registry, digest, producer);
  if (!built) {
    err << built.error().message() << "\n";
    return 1;
  }
  const auto written =
      write_registry_build(*built, std::filesystem::path(receipt_path), spec.output_dir);
  if (!written) {
    err << written.error().message() << "\n";
    return 1;
  }
  out << "atx-research-fields: built " << built->built << " field(s), reused " << built->reused
      << "; receipt " << receipt_path << "; manifest "
      << (spec.output_dir / "manifest.json").string() << "\n";
  return 0;
}

} // namespace

std::span<const std::string_view> engine_field_names() noexcept { return builder_kind_ids(); }

core::Result<BuildSpec> parse_build_spec(std::string_view json_text,
                                         const FieldRegistry *registry) {
  const std::string_view schema = registry == nullptr ? kSpecSchema : kRegistrySpecSchema;
  try {
    const Json j = Json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("schema", std::string{}) != schema) {
      return core::Err(invalid(registry == nullptr
                                   ? "not an atx.research-fields-spec/v1 object"
                                   : "--registry needs an atx.research-fields-spec/v2 object"));
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (!is_known_key(it.key(), registry != nullptr)) {
        return core::Err(invalid("unknown key " + it.key()));
      }
    }
    BuildSpec out;
    const auto role = j.find("role");
    if (role == j.end() || !role->is_object()) {
      return core::Err(invalid("needs a role object {dir, manifest_sha256}"));
    }
    ATX_TRY(const auto role_dir, required_string(*role, "dir"));
    ATX_TRY(out.role_manifest_sha256, required_string(*role, "manifest_sha256"));
    out.role_dir = std::filesystem::path(role_dir);
    ATX_TRY(const auto output_dir, required_string(j, "output_dir"));
    out.output_dir = std::filesystem::path(output_dir);
    ATX_TRY_VOID(parse_fields(j, out));
    if (j.contains("finra")) {
      ATX_TRY(const auto finra, required_string(j, "finra"));
      out.finra = std::filesystem::path(finra);
    }
    if (j.contains("reuse")) {
      ATX_TRY(auto reuse, parse_reuse(j.at("reuse")));
      out.reuse = std::move(reuse);
    }
    const auto planned = plan_fields(out, registry); // every name through its kind's parse
    if (!planned) {
      return core::Err(planned.error());
    }
    return core::Ok(std::move(out));
  } catch (const std::exception &e) {
    return core::Err(invalid(e.what()));
  }
}

core::Result<std::string> build_fields(const BuildSpec &spec, std::string_view spec_sha256,
                                       const ProducerIdentity &producer) {
  try {
    ATX_TRY_VOID(require_output_dir(spec));
    ATX_TRY(const auto role, RoleAxes::load(spec.role_dir, spec.role_manifest_sha256));
    ATX_TRY(const auto plans, plan_fields(spec, nullptr));
    ATX_TRY(Json fields, build_entries(spec, role, plans));
    ATX_TRY_VOID(repin_role(spec));
    const Json receipt = receipt_json(spec, spec_sha256, role, std::move(fields), producer);
    return core::Ok(receipt.dump(2) + "\n");
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::Internal, std::string("atx-research-fields: ") + e.what());
  }
}

core::Result<RegistryBuild> build_registry_fields(const BuildSpec &spec,
                                                  const FieldRegistry &registry,
                                                  std::string_view spec_sha256,
                                                  const ProducerIdentity &producer) {
  try {
    ATX_TRY_VOID(require_output_dir(spec));
    std::error_code ec;
    const bool published = std::filesystem::exists(spec.output_dir / "manifest.json", ec);
    if (published || ec) {
      return core::Err(invalid("output_dir holds a manifest.json (or cannot be checked)"));
    }
    ATX_TRY(const auto role, RoleAxes::load(spec.role_dir, spec.role_manifest_sha256));
    ATX_TRY(const auto plans, plan_fields(spec, &registry));
    std::optional<ReuseOutcome> reuse;
    if (spec.reuse) {
      ATX_TRY(auto outcome, reuse_payloads(*spec.reuse, role, plans, producer, spec.output_dir));
      reuse = std::move(outcome);
    }
    std::vector<FieldPlan> to_build;
    Json reused_names = Json::array();
    for (const FieldPlan &plan : plans) {
      if (reuse && reuse->find(plan.name) != nullptr) {
        reused_names.push_back(plan.name);
      } else {
        to_build.push_back(plan);
      }
    }
    ATX_TRY(const Json built, build_entries(spec, role, to_build));
    ATX_TRY_VOID(repin_role(spec));
    Json receipt = receipt_json(spec, spec_sha256, role, built, producer);
    receipt["registry"] = Json{{"path", registry.path}, {"sha256", registry.sha256}};
    receipt["reused"] = std::move(reused_names);
    RegistryBuild out;
    out.receipt = receipt.dump(2) + "\n";
    ATX_TRY(const auto receipt_sha, core::sha256_hex(std::string_view(out.receipt)));
    const ReuseOutcome *reused = reuse ? &*reuse : nullptr;
    const ManifestInputs inputs{role,     plans,       built,       reused,
                                producer, receipt_sha, spec_sha256, registry};
    ATX_TRY(const auto manifest, engine_manifest(inputs));
    ATX_TRY(out.manifest, manifest_text(manifest));
    out.built = to_build.size();
    out.reused = plans.size() - to_build.size();
    return core::Ok(std::move(out));
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::Internal, std::string("atx-research-fields: ") + e.what());
  }
}

core::Status write_registry_build(const RegistryBuild &build,
                                  const std::filesystem::path &receipt_path,
                                  const std::filesystem::path &output_dir) {
  ATX_TRY_VOID(write_exclusive(receipt_path, build.receipt));
  return publish_exclusive(output_dir / "manifest.json", build.manifest);
}

int research_fields_main(std::span<const std::string_view> args, std::ostream &out,
                         std::ostream &err) {
  Args a;
  if (const int usage_exit = parse_args(args, a, err); usage_exit != 0) {
    return usage_exit;
  }
  const auto text = read_bounded(std::filesystem::path(*a.spec), kSpecLimit);
  if (!text) {
    err << text.error().message() << "\n";
    return 2;
  }
  std::optional<FieldRegistry> registry;
  if (a.registry) {
    auto loaded = load_field_registry(std::filesystem::path(*a.registry));
    if (!loaded) {
      err << loaded.error().message() << "\n";
      return 2;
    }
    registry = std::move(*loaded);
  }
  const auto spec = parse_build_spec(*text, registry ? &*registry : nullptr);
  if (!spec) {
    err << spec.error().message() << "\n";
    return 2;
  }
  const auto digest = core::sha256_hex(std::string_view(*text));
  const auto producer = current_producer();
  if (!digest || !producer) {
    err << (!digest ? digest.error().message() : producer.error().message()) << "\n";
    return 1;
  }
  return registry ? run_registry(*spec, *registry, *digest, *producer, *a.receipt, out, err)
                  : run_plain(*spec, *digest, *producer, *a.receipt, out, err);
}

} // namespace atx::engine::research::fields
