#include "atx/engine/research/fields/russell_recon_field.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// The declared text; atx-engine/tools/field_registry.json's russell_recon row carries it verbatim
// as spec_text (formula fingerprint pinned by the registry check and the AL-SIG gtests).
[[nodiscard]] FieldSpec make_russell_recon_spec() {
  FieldSpec s;
  s.name = "russell_recon";
  s.group = "russell_proxy";
  s.formula_id = "russell-recon-r1k-r2k-move-v1";
  s.revision = 1;
  s.min_history = "two Russell proxy reconstitutions (the stage holds 2018-06 on: the first "
                  "predicted move is the 2019 reconstitution's)";
  FieldDefinition &d = s.definition;
  d.units = "predicted Russell 1000 / 2000 boundary move of the line at the upcoming June "
            "reconstitution: +1 R1000 -> R2000, -1 R2000 -> R1000, 0 a line that stays in the "
            "R1000 or in the R2000";
  d.clock = "russell-proxy-rank-date-v1: reconstitution R (the indexes constituents rows of index "
            "r1000 or r2000 with recon_date R) is visible at V(R) = the maximum available_at (rank "
            "date 22:00 UTC) of its rows and of the prior reconstitution's rows; session t reads "
            "the latest R with V(R) < date(t-1) 22:00 UTC while date(t) < recon_date(R); session "
            "0 -> NaN";
  d.staleness = "NaN outside the event window (from the first session at which the "
                "reconstitution is visible to the last session dated before its recon_date); NaN "
                "for a line not in the r1000 or r2000 rows of both R and the prior "
                "reconstitution, for a line in both indexes of one snapshot, and at the first "
                "reconstitution of the stage (no prior snapshot)";
  d.source_columns = {"constituents.index_id", "constituents.security_id",
                      "constituents.rank_date", "constituents.recon_date",
                      "constituents.available_at"};
  d.definition = "Russell 1000 / 2000 reconstitution price pressure (Chang, Hong and Liskovich "
                 "2015, RFS 28(1)) on the house Russell proxy (indexes stage, rule "
                 "russell-proxy-v1: ranked on the May rank day, the last business day of April "
                 "from 2023, effective after the fourth Friday of June). snapshot(R) = the r1000 "
                 "and r2000 rows with recon_date R; the prior reconstitution R' = the latest "
                 "recon_date before R. Value of a line: +1 when in r1000 at R' and in r2000 at R "
                 "(predicted R2000 addition, R1000 deletion), -1 when in r2000 at R' and in r1000 "
                 "at R (predicted R1000 addition), 0 when in the same one of the two at R' and R. "
                 "Rows available on or after the research seal are dropped. Formula id "
                 "russell-recon-r1k-r2k-move-v1.";
  d.point_in_time = true;
  d.domain = std::array<f64, 2>{-1.0, 1.0};
  s.caveats = {
      "a house proxy of the Russell lists (the official FTSE Russell lists are licensed and not "
      "used): the proxy's own rank and band rules, no float adjustment, IPO and spin-off "
      "additions not modelled",
      "the index ids r1000 and r2000 of the constituents file are declared, not discovered: "
      "another spelling is refused with the ids seen"};
  return s;
}

[[nodiscard]] bool holds(const std::vector<i64> &sorted_ids, i64 id) {
  return std::binary_search(sorted_ids.begin(), sorted_ids.end(), id);
}

[[nodiscard]] core::Error refused(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "atx-research-fields spec: " + std::move(message));
}

struct NanReasons {
  u64 outside_window{};
  u64 not_a_boundary_line{};
};

} // namespace

const FieldSpec &russell_recon_spec() {
  static const FieldSpec spec = make_russell_recon_spec();
  return spec;
}

std::vector<ReconMove> recon_moves(const RussellSnapshots &snapshots, std::span<const i64> ids) {
  std::vector<ReconMove> out;
  for (usize k = 1; k < snapshots.recons.size(); ++k) {
    const RussellRecon &prior = snapshots.recons[k - 1];
    const RussellRecon &next = snapshots.recons[k];
    ReconMove move;
    move.recon_day = next.recon_day;
    move.visible_ns = std::max(prior.visible_ns, next.visible_ns);
    move.value.assign(ids.size(), kNaN);
    for (usize j = 0; j < ids.size(); ++j) {
      const bool was_1000 = holds(prior.r1000, ids[j]);
      const bool was_2000 = holds(prior.r2000, ids[j]);
      const bool is_1000 = holds(next.r1000, ids[j]);
      const bool is_2000 = holds(next.r2000, ids[j]);
      if ((was_1000 && was_2000) || (is_1000 && is_2000)) {
        ++move.conflict;
      } else if (was_1000 && is_2000) {
        move.value[j] = 1.0;
        ++move.down;
      } else if (was_2000 && is_1000) {
        move.value[j] = -1.0;
        ++move.up;
      } else if ((was_1000 && is_1000) || (was_2000 && is_2000)) {
        move.value[j] = 0.0;
      }
    }
    out.push_back(std::move(move));
  }
  return out;
}

std::optional<usize> russell_recon_row(std::span<const ReconMove> moves, i64 day, i64 mark,
                                       std::span<f64> out) {
  std::optional<usize> active;
  for (usize k = 0; k < moves.size(); ++k) { // the latest visible reconstitution
    if (moves[k].visible_ns < mark &&
        (!active || moves[k].recon_day > moves[*active].recon_day)) {
      active = k;
    }
  }
  if (active && day >= moves[*active].recon_day) {
    active.reset(); // the reconstitution has taken place: outside its window
  }
  if (!active) {
    std::fill(out.begin(), out.end(), kNaN);
    return std::nullopt;
  }
  const std::vector<f64> &value = moves[*active].value;
  std::copy(value.begin(), value.end(), out.begin());
  return active;
}

core::Result<RussellReconField> build_russell_recon(const RussellSnapshots &snapshots,
                                                    const PinnedStage &stage,
                                                    const RoleAxes &role,
                                                    const std::filesystem::path &output_dir) {
  const usize n = role.instruments();
  const std::vector<i64> marks = previous_session_marks(role.days());
  const std::vector<ReconMove> moves = recon_moves(snapshots, role.ids());
  ATX_TRY(auto writer, FieldWriter::create(output_dir, russell_recon_spec().name, role));
  std::vector<f64> row(n);
  NanReasons reasons;
  for (usize t = 0; t < role.dates(); ++t) {
    const auto active = russell_recon_row(moves, role.days()[t], marks[t], row);
    ATX_TRY_VOID(writer.write(row));
    const auto member = role.member_row(t);
    for (usize j = 0; j < n; ++j) {
      if (member[j] == 0) {
        continue;
      }
      if (!active) {
        ++reasons.outside_window;
      } else if (std::isnan(row[j])) { // NaN inside a window
        ++reasons.not_a_boundary_line;
      }
    }
  }
  ATX_TRY(auto written, writer.close());
  Json windows = Json::array();
  for (const ReconMove &m : moves) {
    windows.push_back(Json{{"recon_date", iso_date(m.recon_day)},
                           {"visible_at", iso_instant(m.visible_ns)},
                           {"down_moves_on_axis", m.down},
                           {"up_moves_on_axis", m.up},
                           {"conflicts_on_axis", m.conflict}});
  }
  Json stage_files = Json::array();
  for (const SourceRecord &f : stage.sources()) {
    stage_files.push_back(Json{{"path", f.path}, {"bytes", f.bytes}, {"sha256", f.sha256}});
  }
  RussellReconField out;
  out.field = std::move(written);
  out.checks = Json{{"visibility_rule", std::string(kStageVisibilityRule)},
                    {"seal", std::string(data::kSealBeginDate)},
                    {"window", std::string(data::kResearchWindowId)},
                    {"stage", stage.check()},
                    {"stage_files", std::move(stage_files)},
                    {"russell", russell_checks(snapshots)},
                    {"windows", std::move(windows)},
                    {"nan_reasons_member_cells",
                     Json{{"outside_window", reasons.outside_window},
                          {"not_a_boundary_line", reasons.not_a_boundary_line}}}};
  return core::Ok(std::move(out));
}

core::Result<FieldPlan> parse_russell_recon_kind(std::string_view field,
                                                 const nlohmann::json &options,
                                                 const BuildSpec &spec) {
  const FieldSpec &s = russell_recon_spec();
  ATX_TRY_VOID(check_kind_request(s, field, options));
  if (!spec.indexes) {
    return core::Err(refused("russell_recon needs indexes {dir, manifest_sha256}"));
  }
  FieldPlan plan;
  plan.name = s.name;
  plan.spec = &s;
  plan.options = options;
  plan.inputs = {spec.indexes->dir / "manifest.json"};
  return core::Ok(std::move(plan));
}

core::Result<KindOutput> build_russell_recon_kind(const FieldPlan & /*plan*/, BuildContext &ctx) {
  if (!ctx.spec.indexes) {
    return core::Err(refused("russell_recon needs indexes {dir, manifest_sha256}"));
  }
  if (!ctx.sources.indexes) { // pinned once per run, shared by every kind that reads it
    ATX_TRY(auto stage, PinnedStage::open(*ctx.spec.indexes, kIndexesStage));
    ctx.sources.indexes = std::move(stage);
  }
  PinnedStage &stage = *ctx.sources.indexes;
  ATX_TRY(const auto snapshots, read_russell_snapshots(stage));
  ATX_TRY(auto built, build_russell_recon(snapshots, stage, ctx.role, ctx.spec.output_dir));
  KindOutput out;
  out.field = std::move(built.field);
  out.sources = {stage.manifest_source()};
  out.blocks = Json{{"source_checks", std::move(built.checks)},
                    {"extra", Json{{"visibility_rule", std::string(kStageVisibilityRule)},
                                   {"stage_manifest_sha256", stage.manifest_source().sha256},
                                   {"seal_date", std::string(data::kSealBeginDate)}}}};
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
