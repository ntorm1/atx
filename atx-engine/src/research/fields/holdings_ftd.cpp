// ftd_shares_ratio21 (research_fields_holdings.py build_ftd; P9 lane A4). Declared in
// holdings_fields.hpp.
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/holdings_fields.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
// The year files a role reads: the year of (first session - staleness - 60 days) .. the last one.
constexpr i64 kFtdYearMarginDays = 60;
constexpr i64 kCalendarGapDays = 5;

[[nodiscard]] core::Error refused(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "atx-research-fields spec: " + std::move(message));
}

[[nodiscard]] u64 members_of(std::span<const u8> member) noexcept {
  return static_cast<u64>(std::count_if(member.begin(), member.end(),
                                        [](u8 m) { return m != 0; }));
}

[[nodiscard]] Json records_json(std::span<const SourceRecord> records) {
  Json out = Json::array();
  for (const SourceRecord &r : records) {
    out.push_back(Json{{"path", r.path}, {"bytes", r.bytes}, {"sha256", r.sha256}});
  }
  return out;
}

} // namespace

FtdIndexer::FtdIndexer(const RoleAxes &role) : role_(&role) {}

void FtdIndexer::add(const FtdRows &rows, std::string_view name) {
  names_.emplace_back(name);
  const usize count = rows.settlement_day.size();
  rows_ += static_cast<u64>(count);
  for (usize i = 0; i < count; ++i) {
    const i64 available = rows.available_ns[i];
    if (data::is_sealed(available)) { // a null clock (kNeverNs) is sealed too
      ++rows_sealed_;
      continue;
    }
    const i64 day = rows.settlement_day[i];
    const auto slot = visible_.try_emplace(day, kBeforeAllNs).first;
    slot->second = std::max(slot->second, available);
    const bool collision = rows.collision[i] != 0;
    rows_collision_ += collision ? 1U : 0U;
    const i64 sid = rows.security_id[i];
    rows_unmapped_ += sid <= 0 ? 1U : 0U;
    const auto column = sid > 0 ? role_->column_of(sid) : std::nullopt;
    if (column && !collision && std::isfinite(rows.quantity[i])) {
      ++rows_on_role_;
      kept_.push_back(Kept{day, *column, rows.quantity[i]});
    }
  }
}

core::Result<FtdPanel> FtdIndexer::finish() {
  if (visible_.empty()) {
    return core::Err(refused("ftd: no settlement date before the seal in the stage's year files"));
  }
  const usize n = role_->instruments();
  FtdPanel panel;
  panel.columns = n;
  i64 running = kBeforeAllNs;
  for (const auto &[day, available] : visible_) { // ascending days: the settlement calendar
    panel.calendar.push_back(day);
    running = std::max(running, available);
    panel.prefix_visible_ns.push_back(running);
  }
  const usize dates = panel.calendar.size();
  panel.cumulative.assign((dates + 1) * n, 0.0);
  std::vector<u64> cells;
  cells.reserve(kept_.size());
  for (const Kept &row : kept_) { // np.add.at(cum, (k + 1, col), qty): row order
    const auto at = std::lower_bound(panel.calendar.begin(), panel.calendar.end(), row.day);
    const usize k = static_cast<usize>(std::distance(panel.calendar.begin(), at));
    panel.cumulative[(k + 1) * n + row.column] += row.quantity;
    cells.push_back(static_cast<u64>(k) * static_cast<u64>(n) + static_cast<u64>(row.column));
  }
  std::sort(cells.begin(), cells.end());
  u64 summed = 0;
  for (usize i = 0; i < cells.size();) {
    usize j = i + 1;
    while (j < cells.size() && cells[j] == cells[i]) {
      ++j;
    }
    summed += j - i > 1 ? 1U : 0U;
    i = j;
  }
  for (usize k = 1; k <= dates; ++k) { // numpy's sequential axis-0 cumsum
    for (usize j = 0; j < n; ++j) {
      panel.cumulative[k * n + j] = panel.cumulative[(k - 1) * n + j] + panel.cumulative[k * n + j];
    }
  }
  u64 gaps = 0;
  for (usize k = 1; k < dates; ++k) {
    gaps += panel.calendar[k] - panel.calendar[k - 1] > kCalendarGapDays ? 1U : 0U;
  }
  panel.checks = Json{{"files", Json(names_)},
                      {"rows", rows_},
                      {"rows_sealed", rows_sealed_},
                      {"rows_on_role", rows_on_role_},
                      {"rows_collision_excluded", rows_collision_},
                      {"rows_unmapped", rows_unmapped_},
                      {"role_rows_summed_duplicates", summed},
                      {"settlement_calendar", Json{{"dates", dates},
                                                   {"first", iso_date(panel.calendar.front())},
                                                   {"last", iso_date(panel.calendar.back())},
                                                   {"gaps_over_5_days", gaps}}}};
  return core::Ok(std::move(panel));
}

void ftd_row(const FtdPanel &panel, const RoleAxes &role, const RunPayload &shares_out, usize t,
             i64 mark, std::span<const u8> member, FtdReasons &reasons, std::span<f64> out) {
  std::fill(out.begin(), out.end(), kNaN);
  const u64 members = members_of(member);
  // S*(t): the latest settlement date whose whole prefix is visible before the mark.
  const auto visible = std::lower_bound(panel.prefix_visible_ns.begin(),
                                        panel.prefix_visible_ns.end(), mark);
  const usize visible_dates =
      static_cast<usize>(std::distance(panel.prefix_visible_ns.begin(), visible));
  if (visible_dates == 0) {
    reasons.no_visible_window += members;
    return;
  }
  const usize anchor = visible_dates - 1;
  const i64 settle = panel.calendar[anchor];
  const auto days = role.days();
  if (days[t] - settle > kFtdStaleDays) {
    reasons.stale += members;
    return;
  }
  if (anchor + 1 < kFtdWindow) {
    reasons.short_history += members;
    return;
  }
  // shares_out at the last role session on or before S*(t) (none: every line NaN)
  const auto after = std::upper_bound(days.begin(), days.end(), settle);
  const usize sessions_through = static_cast<usize>(std::distance(days.begin(), after));
  const usize n = panel.columns;
  const usize hi = (anchor + 1) * n;
  const usize lo = (anchor + 1 - kFtdWindow) * n;
  for (usize j = 0; j < n; ++j) {
    const f64 so = sessions_through == 0 ? kNaN : shares_out.row(sessions_through - 1)[j];
    if (!(std::isfinite(so) && so > 0.0)) {
      reasons.shares_out_nan += member[j] != 0 ? 1U : 0U;
      continue;
    }
    const f64 sum = panel.cumulative[hi + j] - panel.cumulative[lo + j];
    out[j] = sum / so;
  }
}

core::Result<HoldingsField> build_ftd_shares_ratio21(PinnedStage &stage,
                                                     const RunPayload &shares_out,
                                                     const RoleAxes &role,
                                                     const std::filesystem::path &output_dir) {
  const FieldSpec &spec = ftd_shares_ratio21_spec();
  if (shares_out.dates() != role.dates() || shares_out.columns() != role.instruments()) {
    return core::Err(refused("ftd_shares_ratio21: shares_out is not on the role's shape"));
  }
  const auto days = role.days();
  const i64 lo = year_of(days.front() - kFtdStaleDays - kFtdYearMarginDays);
  const std::vector<std::string> files = stage_year_files(stage, "ftd.parquet", lo,
                                                          year_of(days.back()));
  if (files.empty()) {
    return core::Err(refused("ftd: no year file of the stage overlaps the role"));
  }
  FtdIndexer indexer(role);
  for (const std::string &rel : files) {
    ATX_TRY(const auto rows, read_ftd_rows(stage, rel)); // one year at a time
    indexer.add(rows, rel);
  }
  ATX_TRY(const auto panel, indexer.finish());
  const std::vector<i64> marks = previous_session_marks(days);
  ATX_TRY(auto writer, FieldWriter::create(output_dir, spec.name, role));
  std::vector<f64> row(role.instruments());
  FtdReasons reasons;
  for (usize t = 0; t < role.dates(); ++t) {
    ftd_row(panel, role, shares_out, t, marks[t], role.member_row(t), reasons, row);
    ATX_TRY_VOID(writer.write(row));
  }
  ATX_TRY(auto written, writer.close());
  const std::vector<SourceRecord> read = stage.sources();
  HoldingsField out;
  out.field = std::move(written);
  out.sources = {stage.manifest_source(), shares_out.source()};
  out.extra = Json{{"visibility_rule", std::string(kStageVisibilityRule)},
                   {"stage_manifest_sha256", Json{{"ftd", stage.manifest_source().sha256}}},
                   {"seal_date", std::string(data::kSealBeginDate)},
                   {"depends_on", Json::array({"shares_out"})},
                   {"nan_reasons_member_cells",
                    Json{{"no_visible_window", reasons.no_visible_window},
                         {"stale", reasons.stale},
                         {"shares_out_nan", reasons.shares_out_nan},
                         {"short_history", reasons.short_history}}}};
  out.source_checks =
      Json{{"visibility_rule", std::string(kStageVisibilityRule)},
           {"seal", std::string(data::kSealBeginDate)},
           {"window", std::string(data::kResearchWindowId)},
           {"stage", Json{{"ftd", stage.check()}}},
           {"stage_files", records_json(std::span<const SourceRecord>(read).subspan(1))},
           {"ftd", panel.checks}};
  return core::Ok(std::move(out));
}

core::Result<FieldPlan> parse_ftd_kind(std::string_view field, const nlohmann::json &options,
                                       const BuildSpec &spec) {
  const FieldSpec &s = ftd_shares_ratio21_spec();
  ATX_TRY_VOID(check_kind_request(s, field, options));
  if (!spec.ftd || !spec.shares_out) {
    return core::Err(refused(s.name + " needs ftd {dir, manifest_sha256} and shares_out "
                                      "{path, sha256} (this run's shares_out.f64)"));
  }
  FieldPlan plan;
  plan.name = s.name;
  plan.spec = &s;
  plan.options = options;
  plan.inputs = {spec.ftd->dir / "manifest.json", spec.shares_out->path};
  return core::Ok(std::move(plan));
}

core::Result<KindOutput> build_ftd_kind(const FieldPlan &plan, BuildContext &ctx) {
  if (!ctx.spec.ftd || !ctx.spec.shares_out) {
    return core::Err(refused(plan.name + " needs ftd and shares_out"));
  }
  if (!ctx.sources.ftd) { // pinned once per run
    ATX_TRY(auto stage, PinnedStage::open(*ctx.spec.ftd, kFtdStage));
    ctx.sources.ftd = std::move(stage);
  }
  if (!ctx.sources.shares_out) { // read once per run, shared by every kind that divides by it
    ATX_TRY(auto payload, RunPayload::load(*ctx.spec.shares_out, ctx.role));
    ctx.sources.shares_out = std::move(payload);
  }
  ATX_TRY(auto built, build_ftd_shares_ratio21(*ctx.sources.ftd, *ctx.sources.shares_out,
                                               ctx.role, ctx.spec.output_dir));
  KindOutput out;
  out.field = std::move(built.field);
  out.sources = std::move(built.sources);
  out.blocks = Json{{"extra", std::move(built.extra)},
                    {"source_checks", std::move(built.source_checks)}};
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
