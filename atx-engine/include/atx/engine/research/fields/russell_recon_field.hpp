#pragma once

// atx::engine::research::fields -- field russell_recon (P9 lane AL-SIG, lit F8; Chang, Hong and
// Liskovich 2015, RFS 28(1)): the predicted Russell 1000 / 2000 boundary move of each line at the
// upcoming June reconstitution, from the house Russell proxy (sources/russell_proxy.hpp), an
// engine-only field (DEC-5: a registry row of kind engine naming this builder kind; no Python
// twin).
//
// For each reconstitution R with a prior one R' (the latest recon_date before R), each role line:
//   +1  in R1000P at R' and in R2000P at R  (predicted R2000 addition = R1000 deletion);
//   -1  in R2000P at R' and in R1000P at R  (predicted R1000 addition = R2000 deletion);
//    0  in the same one of the two at R' and at R;
//   NaN otherwise (not a member of both snapshots, or in both indexes of one snapshot).
// Clock russell-proxy-rank-date-v1: R is visible at V(R) = max(available_at of R's rows and of R''s
// rows); session t shows the latest R with V(R) < date(t-1) 22:00 UTC (stage.hpp visibility rule)
// while date(t) < recon_date(R), NaN otherwise (session 0, before the first visible move, after the
// reconstitution day). The rank date precedes the reconstitution by about seven weeks, so each
// year has one window of about 30 sessions.

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_writer.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/sources/russell_proxy.hpp"

namespace atx::engine::research::fields {

// The declared spec (its text is the registry row's spec_text; the fingerprint pins both).
[[nodiscard]] const FieldSpec &russell_recon_spec();

// One reconstitution with a prior one: its day, visibility and the value of each id of `ids`.
struct ReconMove {
  i64 recon_day{};
  i64 visible_ns{};
  std::vector<f64> value;
  u64 down{};     // +1 lines on the axis
  u64 up{};       // -1 lines on the axis
  u64 conflict{}; // lines in both indexes of one snapshot
};

// The moves of every reconstitution after the first, on the ascending id axis `ids`.
[[nodiscard]] std::vector<ReconMove> recon_moves(const RussellSnapshots &snapshots,
                                                 std::span<const i64> ids);

// Row of a session dated `day` whose visibility mark is `mark` (stage.hpp
// previous_session_marks): `out` (ids.size() values) gets the active move's values or NaN. Returns
// the active move's index, nullopt outside every window.
std::optional<usize> russell_recon_row(std::span<const ReconMove> moves, i64 day, i64 mark,
                                       std::span<f64> out);

struct RussellReconField {
  WrittenField field;
  nlohmann::json checks; // source_checks block
};

// Writes output_dir/russell_recon.f64 on `role` (output_dir exists).
[[nodiscard]] core::Result<RussellReconField>
build_russell_recon(const RussellSnapshots &snapshots, const PinnedStage &stage,
                    const RoleAxes &role, const std::filesystem::path &output_dir);

// The builder kind (registry.hpp): the field needs the spec's "indexes" stage; its entry's source
// is the stage manifest (which pins every stage file read).
[[nodiscard]] core::Result<FieldPlan> parse_russell_recon_kind(std::string_view field,
                                                               const nlohmann::json &options,
                                                               const BuildSpec &spec);
[[nodiscard]] core::Result<KindOutput> build_russell_recon_kind(const FieldPlan &plan,
                                                                BuildContext &ctx);

} // namespace atx::engine::research::fields
