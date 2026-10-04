#include "atx/engine/research/fields/registry.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "atx/engine/research/fields/vendor_fields.hpp"
#include "atx/engine/research/fields/volume_mean_field.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;
using Paths = std::vector<std::filesystem::path>;

[[nodiscard]] core::Error refused(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "atx-research-fields spec: " + std::move(message));
}

[[nodiscard]] Json text_or_null(const std::optional<std::string> &value) {
  return value ? Json(*value) : Json(nullptr);
}

// A ported kind builds exactly its own field (its spec text names it) and accepts only the option
// {"group": <its spec group>}.
[[nodiscard]] core::Status check_request(const FieldSpec &spec, std::string_view field,
                                         const Json &options) {
  if (field != spec.name) {
    return core::Err(refused("kind " + spec.name + " builds field " + spec.name + " only, not " +
                             std::string(field)));
  }
  if (!options.is_object()) {
    return core::Err(refused("kind " + spec.name + ": options must be an object"));
  }
  for (auto it = options.begin(); it != options.end(); ++it) {
    if (it.key() != "group") {
      return core::Err(refused("kind " + spec.name + " takes no option " + it.key()));
    }
    if (!it->is_string() || it->get_ref<const std::string &>() != spec.group) {
      return core::Err(refused("kind " + spec.name + ": option group must be " + spec.group));
    }
  }
  return core::Ok();
}

[[nodiscard]] FieldPlan plan_of(const FieldSpec &spec, const Json &options, Paths inputs) {
  FieldPlan plan;
  plan.name = spec.name;
  plan.spec = &spec;
  plan.options = options;
  plan.inputs = std::move(inputs);
  return plan;
}

// ---- vol_126: the role's volume.f64 and present.u8, in the entry's source order.

[[nodiscard]] core::Result<FieldPlan> parse_vol_126(std::string_view field, const Json &options,
                                                    const BuildSpec &spec) {
  const FieldSpec &s = vol_126_spec();
  ATX_TRY_VOID(check_request(s, field, options));
  return core::Ok(
      plan_of(s, options, Paths{spec.role_dir / "volume.f64", spec.role_dir / "present.u8"}));
}

[[nodiscard]] core::Result<KindOutput> build_vol_126_kind(const FieldPlan & /*plan*/,
                                                          BuildContext &ctx) {
  ATX_TRY(auto built, build_vol_126(ctx.role, ctx.spec.output_dir));
  KindOutput out;
  out.field = std::move(built.field);
  out.sources = std::move(built.sources);
  return core::Ok(std::move(out));
}

// ---- si_shares / si_dtc: the as-of CSV, the producer receipt and the dissemination schedule.

[[nodiscard]] core::Result<FieldPlan> parse_finra(std::string_view kind, std::string_view field,
                                                  const Json &options, const BuildSpec &spec) {
  const FieldSpec *s = finra_spec(kind);
  if (s == nullptr) {
    return core::Err(refused("no FINRA field " + std::string(kind)));
  }
  ATX_TRY_VOID(check_request(*s, field, options));
  if (!spec.finra) {
    return core::Err(refused("si_shares / si_dtc need finra"));
  }
  const std::filesystem::path &root = *spec.finra;
  return core::Ok(plan_of(*s, options,
                          Paths{root / "asof" / (s->name + ".csv"), root / "asof" / "manifest.json",
                                root / "dissemination_schedule.csv"}));
}

[[nodiscard]] core::Result<FieldPlan> parse_si_shares(std::string_view field, const Json &options,
                                                      const BuildSpec &spec) {
  return parse_finra("si_shares", field, options, spec);
}

[[nodiscard]] core::Result<FieldPlan> parse_si_dtc(std::string_view field, const Json &options,
                                                   const BuildSpec &spec) {
  return parse_finra("si_dtc", field, options, spec);
}

[[nodiscard]] Json finra_blocks(const FinraField &built) {
  const VintageRisk &v = built.vintage;
  const FinraStats &s = built.stats;
  return Json{
      {"coverage",
       Json{{"vintage_risk",
             Json{{"rule", v.rule},
                  {"finite_member_cells", v.finite_member_cells},
                  {"last_session_with_republished_visible_cell",
                   text_or_null(v.last_session_with_republished_visible_cell)},
                  {"first_session_vintage_safe", text_or_null(v.first_session_vintage_safe)}}}}},
      {"source_checks", Json{{"rows_total", s.rows_total},
                             {"rows_sealed_dropped", s.rows_sealed},
                             {"rows_matched_axis", s.rows_matched_axis},
                             {"rows_ignored_unknown_id", s.rows_ignored_unknown_id},
                             {"max_stale_days", s.max_stale_days}}},
      {"extra", Json{{"vintage_safe_from", text_or_null(built.vintage_safe_from)}}}};
}

[[nodiscard]] core::Result<KindOutput> build_finra_kind(const FieldPlan &plan, BuildContext &ctx) {
  if (!ctx.spec.finra) {
    return core::Err(refused("si_shares / si_dtc need finra"));
  }
  if (!ctx.sources.finra_schedule) { // read once per run, shared by both FINRA kinds
    ATX_TRY(auto schedule, read_dissemination_schedule(*ctx.spec.finra));
    ctx.sources.finra_schedule = std::move(schedule);
  }
  ATX_TRY(auto built, build_finra_field(plan.name, *ctx.spec.finra, *ctx.sources.finra_schedule,
                                        ctx.role, ctx.spec.output_dir));
  KindOutput out;
  out.blocks = finra_blocks(built);
  out.field = std::move(built.field);
  out.sources = std::move(built.sources);
  return core::Ok(std::move(out));
}

// ---- the vendor-panel kinds (P9 A3): the price_source file first, then (ohlc) the role's close,
// raw close and presence, in the entry's source order.

[[nodiscard]] core::Result<FieldPlan> parse_vendor(std::string_view kind, std::string_view field,
                                                   const Json &options, const BuildSpec &spec) {
  const FieldSpec *s = vendor_field_spec(kind);
  if (s == nullptr) {
    return core::Err(refused("no vendor-panel field " + std::string(kind)));
  }
  ATX_TRY_VOID(check_request(*s, field, options));
  if (!spec.price_source) {
    return core::Err(refused(s->name + " needs price_source (the role's vendor TickerHistory3)"));
  }
  Paths inputs{*spec.price_source};
  if (reads_role_close(kind)) {
    for (const char *file : {"close.f64", "raw_close.f64", "present.u8"}) {
      inputs.push_back(spec.role_dir / file);
    }
  }
  return core::Ok(plan_of(*s, options, std::move(inputs)));
}

[[nodiscard]] core::Result<FieldPlan> parse_ret_overnight(std::string_view field,
                                                          const Json &options,
                                                          const BuildSpec &spec) {
  return parse_vendor("ret_overnight", field, options, spec);
}

[[nodiscard]] core::Result<FieldPlan> parse_ret_intraday(std::string_view field,
                                                         const Json &options,
                                                         const BuildSpec &spec) {
  return parse_vendor("ret_intraday", field, options, spec);
}

[[nodiscard]] core::Result<FieldPlan> parse_ceq_iss_5y(std::string_view field, const Json &options,
                                                       const BuildSpec &spec) {
  return parse_vendor("ceq_iss_5y", field, options, spec);
}

[[nodiscard]] core::Result<FieldPlan> parse_open_adj(std::string_view field, const Json &options,
                                                     const BuildSpec &spec) {
  return parse_vendor("open_adj", field, options, spec);
}

[[nodiscard]] core::Result<FieldPlan> parse_high_adj(std::string_view field, const Json &options,
                                                     const BuildSpec &spec) {
  return parse_vendor("high_adj", field, options, spec);
}

[[nodiscard]] core::Result<FieldPlan> parse_low_adj(std::string_view field, const Json &options,
                                                    const BuildSpec &spec) {
  return parse_vendor("low_adj", field, options, spec);
}

// Loads the run's vendor panel once, with the union of every vendor plan's request (the plan being
// built included), so the file is hashed and scanned once whatever the field order.
[[nodiscard]] core::Status ensure_vendor_panel(const FieldPlan &plan, BuildContext &ctx) {
  if (ctx.sources.vendor_panel) {
    return core::Ok();
  }
  if (!ctx.spec.price_source) {
    return core::Err(refused(plan.name + " needs price_source"));
  }
  VendorPanelRequest request;
  for (const FieldPlan &p : ctx.plans) {
    if (const auto need = vendor_request(p.name)) {
      request.merge(*need);
    }
  }
  if (const auto need = vendor_request(plan.name)) {
    request.merge(*need);
  }
  ATX_TRY(auto panel, VendorPanel::load(*ctx.spec.price_source, ctx.role, request));
  ctx.sources.vendor_panel = std::move(panel);
  return core::Ok();
}

[[nodiscard]] core::Result<KindOutput> build_vendor_kind(const FieldPlan &plan, BuildContext &ctx) {
  ATX_TRY_VOID(ensure_vendor_panel(plan, ctx));
  ATX_TRY(auto built, build_vendor_field(plan.name, *ctx.sources.vendor_panel, ctx.role,
                                         ctx.spec.output_dir));
  KindOutput out;
  out.field = std::move(built.field);
  out.sources = std::move(built.sources);
  out.blocks = Json{{"extra", std::move(built.extra)},
                    {"source_checks", std::move(built.source_checks)}};
  return core::Ok(std::move(out));
}

// The kind table. Its order is engine_field_names()' (research_fields_cli.hpp).
constexpr std::array<BuilderKind, 9> kKinds{{
    {"si_shares", &parse_si_shares, &build_finra_kind},
    {"si_dtc", &parse_si_dtc, &build_finra_kind},
    {"vol_126", &parse_vol_126, &build_vol_126_kind},
    {"ret_overnight", &parse_ret_overnight, &build_vendor_kind},
    {"ret_intraday", &parse_ret_intraday, &build_vendor_kind},
    {"ceq_iss_5y", &parse_ceq_iss_5y, &build_vendor_kind},
    {"open_adj", &parse_open_adj, &build_vendor_kind},
    {"high_adj", &parse_high_adj, &build_vendor_kind},
    {"low_adj", &parse_low_adj, &build_vendor_kind},
}};

constexpr std::array<std::string_view, kKinds.size()> kKindIds = [] {
  std::array<std::string_view, kKinds.size()> ids{};
  for (usize i = 0; i < kKinds.size(); ++i) {
    ids[i] = kKinds[i].id;
  }
  return ids;
}();

// The v1 rule: a field name names its kind.
[[nodiscard]] core::Result<FieldPlan> plan_named(const std::string &name, const BuildSpec &spec) {
  const BuilderKind *kind = find_builder_kind(name);
  if (kind == nullptr) {
    return core::Err(refused("no engine builder for " + name));
  }
  ATX_TRY(auto plan, kind->parse(name, Json::object(), spec));
  plan.kind = kind;
  return core::Ok(std::move(plan));
}

// The registry rule (K-P9-1 row of kind engine -> its BuilderKind).
[[nodiscard]] core::Result<FieldPlan> plan_registered(const std::string &name,
                                                      const BuildSpec &spec,
                                                      const FieldRegistry &registry) {
  const RegistryRow *row = registry.find(name);
  if (row == nullptr) {
    return core::Err(refused("field " + name + " is not in the field registry"));
  }
  if (row->kind != FieldKind::Engine) {
    return core::Err(refused("field " + name + " is a python row of the field registry"));
  }
  const BuilderKind *kind = find_builder_kind(row->builder);
  if (kind == nullptr) {
    return core::Err(refused("field " + name + ": unknown builder kind " + row->builder));
  }
  if (row->dtype != FieldDtype::F64) {
    return core::Err(refused("field " + name + ": kind " + std::string(kind->id) +
                             " writes f64, the registry declares " +
                             std::string(field_dtype_name(row->dtype))));
  }
  ATX_TRY(auto plan, kind->parse(name, row->options, spec));
  plan.kind = kind;
  ATX_TRY(const auto formula, formula_sha256(*plan.spec));
  if (row->formula_sha256 && *row->formula_sha256 != formula) {
    return core::Err(refused("field " + name + ": the registry's formula_sha256 is not kind " +
                             std::string(kind->id) + "'s (spec text drift)"));
  }
  return core::Ok(std::move(plan));
}

} // namespace

std::span<const BuilderKind> builder_kinds() noexcept { return kKinds; }

std::span<const std::string_view> builder_kind_ids() noexcept { return kKindIds; }

const BuilderKind *find_builder_kind(std::string_view id) noexcept {
  const auto it = std::find_if(kKinds.begin(), kKinds.end(),
                               [id](const BuilderKind &kind) { return kind.id == id; });
  return it == kKinds.end() ? nullptr : &*it;
}

core::Result<std::vector<FieldPlan>> plan_fields(const BuildSpec &spec,
                                                 const FieldRegistry *registry) {
  std::vector<FieldPlan> plans;
  plans.reserve(spec.fields.size());
  for (const std::string &name : spec.fields) {
    const bool repeated = std::any_of(plans.begin(), plans.end(),
                                      [&name](const FieldPlan &p) { return p.name == name; });
    if (repeated) {
      return core::Err(refused("field " + name + " named twice"));
    }
    ATX_TRY(auto plan, registry == nullptr ? plan_named(name, spec)
                                           : plan_registered(name, spec, *registry));
    plans.push_back(std::move(plan));
  }
  if (registry != nullptr) { // registration order is manifest order (K-P9-1)
    const auto registered_before = [registry](const FieldPlan &a, const FieldPlan &b) {
      return registry->index_of(a.name).value_or(0) < registry->index_of(b.name).value_or(0);
    };
    std::stable_sort(plans.begin(), plans.end(), registered_before);
  }
  return core::Ok(std::move(plans));
}

} // namespace atx::engine::research::fields
