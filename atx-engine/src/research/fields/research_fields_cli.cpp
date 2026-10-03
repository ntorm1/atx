#include "atx/engine/research/fields/research_fields_cli.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_stats.hpp"
#include "atx/engine/research/fields/file_io.hpp"
#include "atx/engine/research/fields/finra_asof_field.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/volume_mean_field.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr u64 kSpecLimit = 1ULL << 20;
constexpr std::string_view kEngineName = "atx-research-fields";
constexpr std::array<std::string_view, 3> kEngineFields{"si_shares", "si_dtc", "vol_126"};

[[nodiscard]] core::Error invalid(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "atx-research-fields spec: " + std::move(message));
}

[[nodiscard]] bool is_finra(std::string_view name) { return finra_spec(name) != nullptr; }

[[nodiscard]] Json number_or_null(const std::optional<f64> &value) {
  return value ? Json(*value) : Json(nullptr);
}

[[nodiscard]] Json text_or_null(const std::optional<std::string> &value) {
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

[[nodiscard]] core::Result<Json> entry_json(const FieldSpec &spec, const WrittenField &field,
                                            const std::vector<SourceRecord> &sources) {
  ATX_TRY(const auto formula, formula_sha256(spec));
  return core::Ok(Json{{"name", field.name},
                       {"file", field.name + ".f64"},
                       {"bytes", field.bytes},
                       {"sha256", field.sha256},
                       {"formula_sha256", formula},
                       {"sources", sources_json(sources)},
                       {"coverage", coverage_json(field.coverage)}});
}

struct Context {
  const BuildSpec &spec;
  const RoleAxes &role;
  std::optional<DisseminationSchedule> schedule;
};

[[nodiscard]] core::Result<Json> build_finra(std::string_view name, Context &ctx) {
  if (!ctx.schedule) {
    ATX_TRY(auto schedule, read_dissemination_schedule(*ctx.spec.finra));
    ctx.schedule = std::move(schedule);
  }
  ATX_TRY(const auto built,
          build_finra_field(name, *ctx.spec.finra, *ctx.schedule, ctx.role, ctx.spec.output_dir));
  ATX_TRY(Json entry, entry_json(*finra_spec(name), built.field, built.sources));
  entry["coverage"]["vintage_risk"] =
      Json{{"rule", built.vintage.rule},
           {"finite_member_cells", built.vintage.finite_member_cells},
           {"last_session_with_republished_visible_cell",
            text_or_null(built.vintage.last_session_with_republished_visible_cell)},
           {"first_session_vintage_safe", text_or_null(built.vintage.first_session_vintage_safe)}};
  entry["source_checks"] =
      Json{{"rows_total", built.stats.rows_total},
           {"rows_available_on_or_after_2025_dropped", built.stats.rows_sealed},
           {"rows_matched_axis", built.stats.rows_matched_axis},
           {"rows_ignored_unknown_id", built.stats.rows_ignored_unknown_id},
           {"max_stale_days", built.stats.max_stale_days}};
  entry["extra"] = Json{{"vintage_safe_from", text_or_null(built.vintage_safe_from)}};
  return core::Ok(std::move(entry));
}

[[nodiscard]] core::Result<Json> build_one(std::string_view name, Context &ctx) {
  if (is_finra(name)) {
    return build_finra(name, ctx);
  }
  if (name == "vol_126") {
    ATX_TRY(const auto built, build_vol_126(ctx.role, ctx.spec.output_dir));
    return entry_json(vol_126_spec(), built.field, built.sources);
  }
  return core::Err(invalid("no engine builder for " + std::string(name)));
}

[[nodiscard]] core::Result<std::string> required_string(const Json &j, const char *key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string() || it->get<std::string>().empty()) {
    return core::Err(invalid(std::string("needs a non-empty string ") + key));
  }
  return core::Ok(it->get<std::string>());
}

[[nodiscard]] std::string usage() {
  return "usage: atx-research-fields build --spec SPEC.json --receipt RECEIPT.json\n";
}

} // namespace

std::span<const std::string_view> engine_field_names() noexcept { return kEngineFields; }

core::Result<BuildSpec> parse_build_spec(std::string_view json_text) {
  try {
    const Json j = Json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("schema", std::string{}) != kSpecSchema) {
      return core::Err(invalid("not an atx.research-fields-spec/v1 object"));
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      const std::string &key = it.key();
      if (key != "schema" && key != "role" && key != "output_dir" && key != "fields" &&
          key != "finra") {
        return core::Err(invalid("unknown key " + key));
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
    const auto fields = j.find("fields");
    if (fields == j.end() || !fields->is_array() || fields->empty()) {
      return core::Err(invalid("needs a non-empty fields array"));
    }
    for (const auto &f : *fields) {
      if (!f.is_string()) {
        return core::Err(invalid("fields holds a non-string"));
      }
      const std::string name = f.get<std::string>();
      if (std::find(kEngineFields.begin(), kEngineFields.end(), name) == kEngineFields.end()) {
        return core::Err(invalid("no engine builder for " + name));
      }
      if (std::find(out.fields.begin(), out.fields.end(), name) != out.fields.end()) {
        return core::Err(invalid("field " + name + " named twice"));
      }
      out.fields.push_back(name);
    }
    if (j.contains("finra")) {
      ATX_TRY(const auto finra, required_string(j, "finra"));
      out.finra = std::filesystem::path(finra);
    }
    const bool needs_finra = std::any_of(out.fields.begin(), out.fields.end(),
                                         [](const std::string &f) { return is_finra(f); });
    if (needs_finra && !out.finra) {
      return core::Err(invalid("si_shares / si_dtc need finra"));
    }
    return core::Ok(std::move(out));
  } catch (const std::exception &e) {
    return core::Err(invalid(e.what()));
  }
}

core::Result<std::string> build_fields(const BuildSpec &spec, std::string_view spec_sha256) {
  try {
    std::error_code ec;
    if (!std::filesystem::is_directory(spec.output_dir, ec)) {
      return core::Err(invalid("output_dir is not a directory: " + spec.output_dir.string()));
    }
    ATX_TRY(const auto role, RoleAxes::load(spec.role_dir, spec.role_manifest_sha256));
    Context ctx{spec, role, std::nullopt};
    Json fields = Json::array();
    for (const auto &name : spec.fields) {
      ATX_TRY(Json entry, build_one(name, ctx));
      fields.push_back(std::move(entry));
    }
    // The role is re-pinned after the last field: its axes must not have moved under the build.
    const auto repinned = RoleAxes::load(spec.role_dir, spec.role_manifest_sha256);
    if (!repinned) {
      return core::Err(repinned.error());
    }
    const Json receipt{{"schema", kReceiptSchema},
                       {"status", "complete"},
                       {"engine", Json{{"name", kEngineName}, {"fields", Json(spec.fields)}}},
                       {"spec_sha256", std::string(spec_sha256)},
                       {"role", Json{{"dir", record_path(spec.role_dir)},
                                     {"manifest_sha256", role.manifest_sha256()}}},
                       {"output_dir", record_path(spec.output_dir)},
                       {"research_window", std::string(data::kResearchWindowId)},
                       {"seal", std::string(data::kSealBeginDate)},
                       {"fields", std::move(fields)}};
    return core::Ok(receipt.dump(2) + "\n");
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::Internal, std::string("atx-research-fields: ") + e.what());
  }
}

int research_fields_main(std::span<const std::string_view> args, std::ostream &out,
                         std::ostream &err) {
  std::optional<std::string_view> spec_path;
  std::optional<std::string_view> receipt_path;
  if (args.size() < 2 || args[1] != "build") {
    err << usage();
    return 2;
  }
  for (usize i = 2; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if ((arg == "--spec" || arg == "--receipt") && i + 1 < args.size()) {
      (arg == "--spec" ? spec_path : receipt_path) = args[i + 1];
      ++i;
    } else {
      err << "atx-research-fields: unknown argument " << arg << "\n" << usage();
      return 2;
    }
  }
  if (!spec_path || !receipt_path) {
    err << usage();
    return 2;
  }
  const auto text = read_bounded(std::filesystem::path(*spec_path), kSpecLimit);
  if (!text) {
    err << text.error().message() << "\n";
    return 2;
  }
  const auto spec = parse_build_spec(*text);
  if (!spec) {
    err << spec.error().message() << "\n";
    return 2;
  }
  const auto digest = core::sha256_hex(std::string_view(*text));
  if (!digest) {
    err << digest.error().message() << "\n";
    return 1;
  }
  const auto receipt = build_fields(*spec, *digest);
  if (!receipt) {
    err << receipt.error().message() << "\n";
    return 1;
  }
  const auto written = write_exclusive(std::filesystem::path(*receipt_path), *receipt);
  if (!written) {
    err << written.error().message() << "\n";
    return 1;
  }
  out << "atx-research-fields: built " << spec->fields.size() << " field(s); receipt "
      << *receipt_path << "\n";
  return 0;
}

} // namespace atx::engine::research::fields
