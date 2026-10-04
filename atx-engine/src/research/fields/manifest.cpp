#include "atx/engine/research/fields/manifest.hpp"

#include <exception>
#include <utility>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/file_io.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

// The Python builder's manifest statements (prepare_research_fields.py run()), so a consumer reads
// the same statements from either producer.
constexpr std::string_view kCellRule =
    "NaN where the field is not visible at the session decision or the source is absent";
constexpr std::string_view kCoverageBasis =
    "member.u8 cells of the role (member cells with a finite value / member cells)";
constexpr std::string_view kVisibilityMark =
    "every finite cell of every field is known by the session-date 22:00 UTC mark (the role "
    "close clock), before the 23:00 UTC decision";

[[nodiscard]] core::Result<Json> role_block(const RoleAxes &role) {
  ATX_TRY(const auto sessions, role.receipt("sessions.i64"));
  ATX_TRY(const auto ids, role.receipt("ids.u64"));
  ATX_TRY(const auto member, role.receipt("member.u8"));
  const auto &source = role.source_sha256();
  return core::Ok(Json{{"path", record_path(role.directory())},
                       {"manifest_sha256", role.manifest_sha256()},
                       {"sessions_sha256", sessions.sha256},
                       {"ids_sha256", ids.sha256},
                       {"member_sha256", member.sha256},
                       {"dates", role.dates()},
                       {"instruments", role.instruments()},
                       {"score_begin", role.score_begin()},
                       {"score_end", role.score_end()},
                       {"first_session", iso_date(role.days().front())},
                       {"last_session", iso_date(role.days().back())},
                       {"source_sha256", source ? Json(*source) : Json(nullptr)}});
}

[[nodiscard]] Json seal_block() {
  const std::string seal(data::kSealBeginDate);
  return Json{{"exclusive_end", seal},
              {"rule", "every source row available on or after " + seal +
                           " is dropped before use; role sessions asserted < " + seal}};
}

[[nodiscard]] const Json *receipt_entry(const Json &built, const std::string &name) {
  for (const Json &e : built) {
    const auto it = e.find("name");
    if (it != e.end() && it->is_string() && it->get_ref<const std::string &>() == name) {
      return &e;
    }
  }
  return nullptr;
}

// The manifest entry of a field this run built: its receipt entry's payload blocks, its kind's
// declared spec and the engine producer block.
[[nodiscard]] Json built_entry(const FieldPlan &plan, const Json &receipt, const RoleAxes &role,
                               const Json &producer) {
  const FieldSpec &s = *plan.spec;
  const FieldDefinition &d = s.definition;
  Json entry{{"name", plan.name},
             {"file", receipt.at("file")},
             {"dtype", "<f8"},
             {"layout", "date-major"},
             {"shape", Json::array({role.dates(), role.instruments()})},
             {"units", d.units},
             {"clock", d.clock},
             {"staleness", d.staleness},
             {"source_columns", Json(d.source_columns)},
             {"caveats", Json(s.caveats)},
             {"point_in_time", d.point_in_time},
             {"non_pit_aspects", Json(d.non_pit_aspects)},
             {"formula_sha256", receipt.at("formula_sha256")},
             {"sources", receipt.at("sources")},
             {"coverage", receipt.at("coverage")},
             {"sha256", receipt.at("sha256")},
             {"producer", producer}};
  if (d.definition) {
    entry["definition"] = *d.definition;
  }
  if (!s.formula_id.empty()) {
    entry["formula_id"] = s.formula_id;
  }
  if (!s.min_history.empty()) {
    entry["min_history"] = s.min_history;
  }
  const auto extra = receipt.find("extra");
  if (extra != receipt.end() && extra->is_object()) { // the kind's entry keys (Python: extras)
    for (auto it = extra->begin(); it != extra->end(); ++it) {
      entry[it.key()] = it.value();
    }
  }
  return entry;
}

struct Entries {
  Json fields = Json::array();
  Json files = Json::object();
  Json source_checks = Json::object();
  Json non_point_in_time = Json::array();
};

[[nodiscard]] core::Result<Entries> entries_of(const ManifestInputs &in) {
  const Json producer = producer_block(in.producer, in.receipt_sha256);
  Entries out;
  for (const FieldPlan &plan : in.plans) {
    const std::string file = plan.name + ".f64";
    const ReusedField *reused = in.reuse == nullptr ? nullptr : in.reuse->find(plan.name);
    Json entry;
    if (reused != nullptr) {
      entry = reused->entry;
      out.files[file] = Json{{"bytes", reused->payload.bytes}, {"sha256", reused->payload.sha256}};
      if (!reused->source_checks.is_null()) {
        out.source_checks[plan.name] = reused->source_checks;
      }
    } else {
      const Json *receipt = receipt_entry(in.built, plan.name);
      if (receipt == nullptr) {
        return core::Err(core::ErrorCode::InvalidArgument,
                         "research fields manifest: no built or reused entry for " + plan.name);
      }
      entry = built_entry(plan, *receipt, in.role, producer);
      out.files[file] = Json{{"bytes", receipt->at("bytes")}, {"sha256", receipt->at("sha256")}};
      const auto checks = receipt->find("source_checks");
      if (checks != receipt->end()) {
        out.source_checks[plan.name] = *checks;
      }
    }
    const auto pit = entry.find("point_in_time");
    if (pit != entry.end() && pit->is_boolean() && !pit->get<bool>()) {
      out.non_point_in_time.push_back(plan.name);
    }
    out.fields.push_back(std::move(entry));
  }
  return core::Ok(std::move(out));
}

} // namespace

core::Result<Json> engine_manifest(const ManifestInputs &in) {
  try {
    ATX_TRY(Json role_json, role_block(in.role));
    ATX_TRY(Entries entries, entries_of(in));
    const ProducerIdentity &p = in.producer;
    Json manifest{
        {"schema", std::string(kFieldsManifestSchema)},
        {"status", "complete"},
        {"role", std::move(role_json)},
        {"instrument_namespace", "spiderrock.securityID"},
        {"seal", seal_block()},
        {"cell_rule", std::string(kCellRule)},
        {"coverage_basis", std::string(kCoverageBasis)},
        {"visibility_mark", std::string(kVisibilityMark)},
        {"non_point_in_time_fields", std::move(entries.non_point_in_time)},
        {"fields", std::move(entries.fields)},
        {"files", std::move(entries.files)},
        {"source_checks", std::move(entries.source_checks)},
        {"engine", Json{{"name", std::string(kEngineName)},
                        {"exe_sha256", p.exe_sha256},
                        {"git_sha", p.git_sha},
                        {"build_type", p.build_type}}},
        {"receipt_sha256", std::string(in.receipt_sha256)},
        {"spec_sha256", std::string(in.spec_sha256)},
        {"registry", Json{{"path", in.registry.path}, {"sha256", in.registry.sha256}}},
        {"research_window", std::string(data::kResearchWindowId)},
        {"historical_vintage_verified", false},
        {"common_stock_verified", false}};
    if (in.reuse != nullptr) {
      manifest["reuse"] = in.reuse->record;
    }
    return core::Ok(std::move(manifest));
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::Internal,
                     std::string("research fields manifest: ") + e.what());
  }
}

core::Result<std::string> manifest_text(const Json &manifest) {
  try {
    return core::Ok(manifest.dump(2, ' ', true) + "\n");
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     std::string("research fields manifest: ") + e.what());
  }
}

} // namespace atx::engine::research::fields
