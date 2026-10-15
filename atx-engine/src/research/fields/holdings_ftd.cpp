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
constexpr usize kNoDate = std::numeric_limits<usize>::max();

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

// The settlement row of `day` (a calendar date by construction).
[[nodiscard]] usize settlement_row(std::span<const i64> calendar, i64 day) noexcept {
  const auto at = std::lower_bound(calendar.begin(), calendar.end(), day);
  return static_cast<usize>(std::distance(calendar.begin(), at));
}

// The number of (settlement row, line) cells holding more than one fail row.
[[nodiscard]] u64 summed_cells(const FtdPanel &panel) {
  std::vector<usize> seen(panel.columns, kNoDate);
  std::vector<usize> counted(panel.columns, kNoDate);
  u64 out = 0;
  for (usize k = 0; k < panel.calendar.size(); ++k) {
    for (usize r = panel.date_begin[k]; r < panel.date_begin[k + 1]; ++r) {
      const usize c = panel.fail_column[r];
      if (seen[c] != k) {
        seen[c] = k;
      } else if (counted[c] != k) {
        counted[c] = k;
        ++out;
      }
    }
  }
  return out;
}

} // namespace

FtdIndexer::FtdIndexer(const RoleAxes &role) : role_(&role) {}

void FtdIndexer::see(i64 day, i64 available) {
  if (last_ >= visible_.size() || visible_[last_].first != day) {
    const auto it = std::lower_bound(
        visible_.begin(), visible_.end(), day,
        [](const std::pair<i64, i64> &entry, i64 d) { return entry.first < d; });
    last_ = static_cast<usize>(std::distance(visible_.begin(), it));
    if (it == visible_.end() || it->first != day) {
      visible_.insert(it, {day, kBeforeAllNs});
    }
  }
  visible_[last_].second = std::max(visible_[last_].second, available);
}

void FtdIndexer::add(const FtdRows &rows) {
  const usize count = rows.size();
  rows_ += static_cast<u64>(count);
  for (usize i = 0; i < count; ++i) {
    const i64 available = rows.available_ns[i];
    if (data::is_sealed(available)) { // a null clock (kNeverNs) is sealed too
      ++rows_sealed_;
      continue;
    }
    const i64 day = rows.settlement_day[i];
    see(day, available);
    const bool collision = rows.basis[i] == kFtdCollision;
    rows_collision_ += collision ? 1U : 0U;
    const i64 sid = rows.security_id[i];
    rows_unmapped_ += sid <= 0 ? 1U : 0U;
    const auto column = sid > 0 ? role_->column_of(sid) : std::nullopt;
    if (column && !collision && std::isfinite(rows.quantity[i])) {
      kept_day_.push_back(day);
      kept_column_.push_back(static_cast<u32>(*column)); // finish refuses a role wider than u32
      kept_quantity_.push_back(rows.quantity[i]);
    }
  }
}

void FtdIndexer::end_file(std::string_view name) { names_.emplace_back(name); }

core::Result<FtdPanel> FtdIndexer::finish() {
  if (visible_.empty()) {
    return core::Err(refused("ftd: no settlement date before the seal in the stage's year files"));
  }
  const usize n = role_->instruments();
  if (n > std::numeric_limits<u32>::max()) {
    return core::Err(refused("ftd: the role has more lines than a fail row can index"));
  }
  FtdPanel panel;
  panel.columns = n;
  i64 running = kBeforeAllNs;
  for (const auto &[day, available] : visible_) { // ascending days: the settlement calendar
    panel.calendar.push_back(day);
    running = std::max(running, available);
    panel.prefix_visible_ns.push_back(running);
  }
  const usize dates = panel.calendar.size();
  // The fails grouped by settlement row, stable in read order (a counting sort).
  panel.date_begin.assign(dates + 1, 0);
  for (const i64 day : kept_day_) {
    ++panel.date_begin[settlement_row(panel.calendar, day) + 1];
  }
  for (usize k = 0; k < dates; ++k) {
    panel.date_begin[k + 1] += panel.date_begin[k];
  }
  std::vector<usize> cursor(panel.date_begin.begin(), panel.date_begin.end() - 1);
  panel.fail_column.resize(kept_day_.size());
  panel.fail_quantity.resize(kept_day_.size());
  for (usize i = 0; i < kept_day_.size(); ++i) {
    const usize at = cursor[settlement_row(panel.calendar, kept_day_[i])]++;
    panel.fail_column[at] = kept_column_[i];
    panel.fail_quantity[at] = kept_quantity_[i];
  }
  const u64 kept = static_cast<u64>(kept_day_.size());
  kept_day_ = {};
  kept_column_ = {};
  kept_quantity_ = {};
  u64 gaps = 0;
  for (usize k = 1; k < dates; ++k) {
    gaps += panel.calendar[k] - panel.calendar[k - 1] > kCalendarGapDays ? 1U : 0U;
  }
  panel.checks = Json{{"files", Json(names_)},
                      {"rows", rows_},
                      {"rows_sealed", rows_sealed_},
                      {"rows_on_role", kept},
                      {"rows_collision_excluded", rows_collision_},
                      {"rows_unmapped", rows_unmapped_},
                      {"role_rows_summed_duplicates", summed_cells(panel)},
                      {"settlement_calendar", Json{{"dates", dates},
                                                   {"first", iso_date(panel.calendar.front())},
                                                   {"last", iso_date(panel.calendar.back())},
                                                   {"gaps_over_5_days", gaps}}}};
  return core::Ok(std::move(panel));
}

FtdKernel::FtdKernel(const FtdPanel &panel, const RoleAxes &role, const RunPayload &shares_out)
    : panel_(&panel), role_(&role), shares_out_(&shares_out),
      ring_((kFtdWindow + 1) * panel.columns, 0.0), adds_(panel.columns, 0.0),
      sums_(panel.columns, 0.0) {}

std::span<f64> FtdKernel::ring_row(usize prefix_row) noexcept {
  const usize n = panel_->columns;
  return std::span<f64>(ring_).subspan((prefix_row % (kFtdWindow + 1)) * n, n);
}

void FtdKernel::advance() {
  const usize k = next_ - 1; // the settlement row this prefix row adds
  std::fill(adds_.begin(), adds_.end(), 0.0);
  for (usize r = panel_->date_begin[k]; r < panel_->date_begin[k + 1]; ++r) {
    adds_[panel_->fail_column[r]] += panel_->fail_quantity[r]; // np.add.at: read order
  }
  const std::span<const f64> previous = ring_row(next_ - 1);
  const std::span<f64> current = ring_row(next_); // another slot: 22 > 1
  for (usize j = 0; j < current.size(); ++j) { // numpy's sequential axis-0 cumsum
    current[j] = previous[j] + adds_[j];
  }
  ++next_;
}

std::span<const f64> FtdKernel::window(usize anchor) {
  if (sums_valid_ && sums_anchor_ == anchor) {
    return sums_;
  }
  const usize hi = anchor + 1;
  const usize lo = hi - kFtdWindow;
  if (lo + kFtdWindow + 1 < next_) { // prefix row lo was evicted: recompute from row 0
    next_ = 1;
    const std::span<f64> zero = ring_row(0);
    std::fill(zero.begin(), zero.end(), 0.0);
  }
  while (next_ <= hi) {
    advance();
  }
  const std::span<const f64> top = ring_row(hi);
  const std::span<const f64> bottom = ring_row(lo);
  for (usize j = 0; j < sums_.size(); ++j) {
    sums_[j] = top[j] - bottom[j];
  }
  sums_anchor_ = anchor;
  sums_valid_ = true;
  return sums_;
}

void FtdKernel::row(usize t, i64 mark, std::span<const u8> member, FtdReasons &reasons,
                    std::span<f64> out) {
  std::fill(out.begin(), out.end(), kNaN);
  const u64 members = members_of(member);
  // S*(t): the latest settlement date whose whole prefix is visible before the mark.
  const auto &visible_ns = panel_->prefix_visible_ns;
  const auto visible = std::lower_bound(visible_ns.begin(), visible_ns.end(), mark);
  const usize visible_dates = static_cast<usize>(std::distance(visible_ns.begin(), visible));
  if (visible_dates == 0) {
    reasons.no_visible_window += members;
    return;
  }
  const usize anchor = visible_dates - 1;
  const i64 settle = panel_->calendar[anchor];
  const auto days = role_->days();
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
  const std::span<const f64> shares =
      sessions_through == 0 ? std::span<const f64>{} : shares_out_->row(sessions_through - 1);
  const std::span<const f64> sums = window(anchor);
  for (usize j = 0; j < out.size(); ++j) {
    const f64 so = shares.empty() ? kNaN : shares[j];
    if (!(std::isfinite(so) && so > 0.0)) {
      reasons.shares_out_nan += member[j] != 0 ? 1U : 0U;
      continue;
    }
    out[j] = sums[j] / so;
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
  const RowsVisitor<FtdRows> add_batch = [&indexer](const FtdRows &rows) -> core::Status {
    indexer.add(rows);
    return core::Ok();
  };
  for (const std::string &rel : files) { // one row group at a time
    ATX_TRY_VOID(read_short_side<FtdRows>(stage, rel, add_batch));
    indexer.end_file(rel);
  }
  ATX_TRY(const auto panel, indexer.finish());
  const std::vector<i64> marks = previous_session_marks(days);
  ATX_TRY(auto writer, FieldWriter::create(output_dir, spec.name, role));
  FtdKernel kernel(panel, role, shares_out);
  std::vector<f64> row(role.instruments());
  FtdReasons reasons;
  for (usize t = 0; t < role.dates(); ++t) {
    kernel.row(t, marks[t], role.member_row(t), reasons, row);
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
