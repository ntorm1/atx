#pragma once

// atx::engine::research::fields -- builder kinds (contract K-P9-2, kind half): the engine's table
// of field builders, looked up by id, and the plan of one build.
//
//   struct BuilderKind {std::string_view id; ParseFn parse; BuildFn build;}
//
// A kind's parse validates one field request (the field name, the registry row's options, the build
// spec's inputs) and returns its FieldPlan: the declared spec, whose formula fingerprint is the
// formula part of the reuse key, and the input files the build reads, in the order its entry
// records them as sources. A kind's build writes output_dir/<name>.f64 through a FieldWriter and
// returns the payload record, its sources and its own entry blocks (FINRA: source_checks,
// coverage.vintage_risk, extra). An input that several kinds read is loaded once per run into
// BuildContext::sources: the FINRA dissemination schedule, and the shared vendor panel (K-P9-2
// source half, sources/vendor_panel.hpp), loaded by the first vendor kind of the run with the union
// of every vendor plan's request (BuildContext::plans) so the vendor file is hashed and scanned once.
//
// Registered kinds, in table order: si_shares and si_dtc (finra_asof_field.hpp), vol_126
// (volume_mean_field.hpp), then the vendor-panel kinds ret_overnight, ret_intraday, ceq_iss_5y,
// open_adj, high_adj, low_adj (vendor_fields.hpp; they need the spec's price_source). Each builds
// exactly its own field and accepts only the option {"group": <its spec group>} (the producer group
// a registry row records), or no option.
//
// plan_fields: without a registry, a field name names its kind (the ported builders' fields, in
// spec order). With a registry (K-P9-1), each field must be a row of kind engine whose builder is a
// registered kind id, dtype f64 and formula_sha256 (when declared) equal to the kind's fingerprint;
// the plans come back in registration order (= manifest order). Err(InvalidArgument) otherwise.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/engine/research/fields/build_spec.hpp"
#include "atx/engine/research/fields/field_registry.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_writer.hpp"
#include "atx/engine/research/fields/finra_asof_field.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/sources/vendor_panel.hpp"

namespace atx::engine::research::fields {

struct BuilderKind;

struct FieldPlan {
  std::string name;
  const BuilderKind *kind{}; // non-owning: an element of the static kind table
  const FieldSpec *spec{};   // non-owning: the kind's static spec
  nlohmann::json options = nlohmann::json::object(); // the request's options, as given
  std::vector<std::filesystem::path> inputs;          // files the build reads, in source order
};

// Inputs read once per run and shared by every kind that needs them.
struct SharedSources {
  std::optional<DisseminationSchedule> finra_schedule;
  std::optional<VendorPanel> vendor_panel;
};

struct BuildContext {
  const BuildSpec &spec;
  const RoleAxes &role;
  SharedSources sources;
  std::span<const FieldPlan> plans{}; // every plan this run builds (a shared source's union)
};

struct KindOutput {
  WrittenField field;
  std::vector<SourceRecord> sources;
  // The kind's own entry blocks, merged into its receipt entry: a top-level object member is merged
  // one level deep (coverage.vintage_risk), any other value is set.
  nlohmann::json blocks = nlohmann::json::object();
};

using ParseFn = core::Result<FieldPlan> (*)(std::string_view field, const nlohmann::json &options,
                                            const BuildSpec &spec);
using BuildFn = core::Result<KindOutput> (*)(const FieldPlan &plan, BuildContext &ctx);

struct BuilderKind {
  std::string_view id;
  ParseFn parse;
  BuildFn build;
};

// The registered kinds (static storage, table order) and their ids in the same order.
[[nodiscard]] std::span<const BuilderKind> builder_kinds() noexcept;
[[nodiscard]] std::span<const std::string_view> builder_kind_ids() noexcept;

// The kind with this id; nullptr for an unknown id.
[[nodiscard]] const BuilderKind *find_builder_kind(std::string_view id) noexcept;

// The plans of spec.fields, through `registry` when it is non-null (see above). Each plan's kind is
// set. Err(InvalidArgument) for a field no kind builds, a repeated field or a refused request.
[[nodiscard]] core::Result<std::vector<FieldPlan>> plan_fields(const BuildSpec &spec,
                                                               const FieldRegistry *registry);

} // namespace atx::engine::research::fields
