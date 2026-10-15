// regsho_threshold_days63 (research_fields_holdings.py build_regsho; P9 lane A4). Declared in
// holdings_fields.hpp.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <utility>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/holdings_fields.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kMarkets = kRegshoMarkets.size();

[[nodiscard]] core::Error refused(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "atx-research-fields spec: " + std::move(message));
}

[[nodiscard]] u64 members_of(std::span<const u8> member) noexcept {
  return static_cast<u64>(std::count_if(member.begin(), member.end(),
                                        [](u8 m) { return m != 0; }));
}

// The calendar index of `day` when it is a calendar date, else nullopt.
[[nodiscard]] std::optional<usize> calendar_index(std::span<const i64> calendar, i64 day) {
  const auto it = std::lower_bound(calendar.begin(), calendar.end(), day);
  if (it == calendar.end() || *it != day) {
    return std::nullopt;
  }
  return static_cast<usize>(std::distance(calendar.begin(), it));
}

[[nodiscard]] Json records_json(std::span<const SourceRecord> records) {
  Json out = Json::array();
  for (const SourceRecord &r : records) {
    out.push_back(Json{{"path", r.path}, {"bytes", r.bytes}, {"sha256", r.sha256}});
  }
  return out;
}

// The listing market of a FINRA marketClassCode code (-1: a class without a list source).
[[nodiscard]] i64 market_of_class(i64 class_code) noexcept {
  return class_code >= 0 && static_cast<usize>(class_code) < kFinraClassMarket.size()
             ? kFinraClassMarket[static_cast<usize>(class_code)]
             : -1;
}

} // namespace

RegshoIndexer::RegshoIndexer(const RegshoListRows &lists, const RoleAxes &role) : role_(&role) {
  const auto days = role.days();
  const usize count = lists.size();
  std::vector<u8> ok(count, 0);
  std::vector<i64> before;
  for (usize i = 0; i < count; ++i) {
    const bool usable = lists.status[i] >= 0 && !data::is_sealed(lists.available_ns[i]);
    ok[i] = static_cast<u8>(usable ? 1 : 0);
    if (usable && lists.list_day[i] < days.front()) {
      before.push_back(lists.list_day[i]);
    }
  }
  std::sort(before.begin(), before.end());
  before.erase(std::unique(before.begin(), before.end()), before.end());
  const usize keep = std::min(before.size(), kRegshoPrefixLists); // the last 65 list dates
  RegshoPanel &p = panel_;
  p.calendar.assign(before.end() - static_cast<std::ptrdiff_t>(keep), before.end());
  p.prefix = keep;
  p.calendar.insert(p.calendar.end(), days.begin(), days.end());
  const usize dates = p.calendar.size();
  p.columns = role.instruments();
  p.list_visible_ns.assign(kMarkets * dates, kNeverNs);
  p.on_market.assign(dates * p.columns, 0);
  u64 used = 0;
  u64 outside = 0;
  u64 unknown = 0;
  for (usize i = 0; i < count; ++i) {
    if (ok[i] == 0) {
      continue;
    }
    const auto k = calendar_index(p.calendar, lists.list_day[i]);
    const i64 m = lists.market[i];
    outside += k ? 0U : 1U;
    unknown += m < 0 ? 1U : 0U;
    if (!k || m < 0) {
      continue;
    }
    ++used;
    i64 &slot = p.list_visible_ns[static_cast<usize>(m) * dates + *k]; // the earliest list file
    slot = std::min(slot, lists.available_ns[i]);
  }
  Json by_market = Json::object();
  for (usize m = 0; m < kMarkets; ++m) {
    const auto begin = p.list_visible_ns.begin() + static_cast<std::ptrdiff_t>(m * dates);
    by_market[std::string(kRegshoMarkets[m])] = static_cast<u64>(
        std::count_if(begin, begin + static_cast<std::ptrdiff_t>(dates),
                      [](i64 v) { return v < kNeverNs; }));
  }
  p.checks = Json{{"lists_used", used},
                  {"lists_outside_the_window_calendar", outside},
                  {"lists_unknown_market", unknown},
                  {"lists_by_market_on_calendar", std::move(by_market)},
                  {"prefix_sessions", keep}};
}

void RegshoIndexer::add(const ThresholdRows &rows) {
  const RegshoPanel &p = panel_;
  const usize dates = p.calendar.size();
  for (usize i = 0; i < rows.size(); ++i) {
    const i64 sid = rows.security_id[i];
    const i64 m = rows.market[i];
    if (rows.on_list[i] == 0 || sid <= 0 || m < 0 || data::is_sealed(rows.available_ns[i])) {
      continue;
    }
    const auto k = calendar_index(p.calendar, rows.list_day[i]);
    const auto column = role_->column_of(sid);
    if (!k || !column) {
      continue;
    }
    const usize market = static_cast<usize>(m);
    const i64 list = p.list_visible_ns[market * dates + *k]; // before this file's raises
    const i64 available = rows.available_ns[i];
    held_.push_back(Held{*k, *column, market, available, available > list});
  }
}

void RegshoIndexer::end_file(std::string_view name) {
  RegshoPanel &p = panel_;
  const usize dates = p.calendar.size();
  for (const Held &h : held_) { // a row later than its list delays the list
    if (h.late) {
      ++late_rows_;
      i64 &slot = p.list_visible_ns[h.market * dates + h.date];
      slot = std::max(slot, h.available);
    }
  }
  for (const Held &h : held_) { // read order: the last row of a cell wins
    p.on_market[h.date * p.columns + h.column] = static_cast<u8>(h.market + 1);
  }
  rows_used_ += static_cast<u64>(held_.size());
  held_.clear();
  names_.emplace_back(name);
}

RegshoPanel RegshoIndexer::finish() {
  panel_.checks["threshold_files"] = Json(names_);
  panel_.checks["on_list_rows_on_role"] = rows_used_;
  panel_.checks["rows_later_than_their_list"] = late_rows_;
  return std::move(panel_);
}

ListingIndexer::ListingIndexer(const RoleAxes &role) : role_(&role) {}

void ListingIndexer::add(const FinraNameRows &rows) {
  for (usize i = 0; i < rows.size(); ++i) {
    const i64 sid = rows.security_id[i];
    const auto column = sid > 0 ? role_->column_of(sid) : std::nullopt;
    if (!column || data::is_sealed(rows.available_ns[i])) {
      continue;
    }
    rows_.available_ns.push_back(rows.available_ns[i]);
    rows_.dissemination_day.push_back(rows.dissemination_day[i]);
    rows_.column.push_back(*column);
    rows_.market.push_back(market_of_class(rows.market_class[i]));
  }
}

ListingRows ListingIndexer::finish() {
  const usize count = rows_.available_ns.size();
  std::vector<usize> order(count);
  std::iota(order.begin(), order.end(), usize{0});
  std::stable_sort(order.begin(), order.end(), [this](usize a, usize b) {
    return rows_.available_ns[a] < rows_.available_ns[b];
  });
  ListingRows out;
  out.available_ns.reserve(count);
  out.dissemination_day.reserve(count);
  out.column.reserve(count);
  out.market.reserve(count);
  for (const usize i : order) {
    out.available_ns.push_back(rows_.available_ns[i]);
    out.dissemination_day.push_back(rows_.dissemination_day[i]);
    out.column.push_back(rows_.column[i]);
    out.market.push_back(rows_.market[i]);
  }
  rows_ = {};
  return out;
}

RegshoKernel::RegshoKernel(const RegshoPanel &panel, const ListingRows &listing,
                           const RoleAxes &role, LatestRows latest)
    : panel_(&panel), listing_(&listing), role_(&role), latest_(std::move(latest)),
      counts_(panel.columns, 0) {}

core::Result<RegshoKernel> RegshoKernel::create(const RegshoPanel &panel,
                                                const ListingRows &listing,
                                                const RoleAxes &role) {
  ATX_TRY(auto latest, LatestRows::create(listing.available_ns, listing.column,
                                          role.instruments()));
  return core::Ok(RegshoKernel(panel, listing, role, std::move(latest)));
}

void RegshoKernel::row(usize t, i64 mark, std::span<const u8> member, RegshoReasons &reasons,
                       std::span<u64> counts_by_market, std::span<f64> out) {
  std::fill(out.begin(), out.end(), kNaN);
  const RegshoPanel &p = *panel_;
  const usize n = p.columns;
  const usize dates = p.calendar.size();
  const i64 end = static_cast<i64>(p.prefix + t) - 2; // the window ends at session t-2
  if (end - static_cast<i64>(kRegshoWindow) + 1 < 0) {
    reasons.short_calendar += members_of(member);
    return;
  }
  const usize first = static_cast<usize>(end) + 1 - kRegshoWindow;
  // Per window session, which on_market codes count: code m + 1 iff market m's list is visible.
  std::array<std::array<u8, kMarkets + 1>, kRegshoWindow> counts_code{};
  std::array<usize, kMarkets> covered{};
  for (usize w = 0; w < kRegshoWindow; ++w) {
    for (usize m = 0; m < kMarkets; ++m) {
      const bool visible = p.list_visible_ns[m * dates + first + w] < mark;
      counts_code[w][m + 1] = static_cast<u8>(visible ? 1 : 0);
      covered[m] += visible ? 1U : 0U;
    }
  }
  std::fill(counts_.begin(), counts_.end(), 0U);
  for (usize w = 0; w < kRegshoWindow; ++w) { // contiguous panel rows, one list date at a time
    const std::span<const u8> on(p.on_market.data() + (first + w) * n, n);
    const auto &code_counts = counts_code[w];
    for (usize j = 0; j < n; ++j) {
      assert(on[j] <= kMarkets);
      counts_[j] += static_cast<u32>(code_counts[on[j]]);
    }
  }
  const std::span<const i64> latest = latest_.before(mark); // name rows before mark(t-1)
  const i64 day = role_->days()[t];
  for (usize j = 0; j < n; ++j) {
    i64 market = -1; // the line's listing market from its latest visible name row
    if (latest[j] >= 0) {
      const usize r = static_cast<usize>(latest[j]);
      if (day - listing_->dissemination_day[r] <= kRegshoNameMaxAgeDays) {
        market = listing_->market[r];
      }
    }
    if (market < 0) {
      reasons.market_unknown += member[j] != 0 ? 1U : 0U;
      continue;
    }
    const usize mk = static_cast<usize>(market);
    if (covered[mk] < kRegshoMinLists) {
      reasons.market_lists_incomplete += member[j] != 0 ? 1U : 0U;
      continue;
    }
    out[j] = static_cast<f64>(counts_[j]);
    counts_by_market[mk] += member[j] != 0 ? 1U : 0U;
  }
}

core::Result<HoldingsField> build_regsho_threshold_days63(PinnedStage &lists_stage,
                                                          PinnedStage &names_stage,
                                                          const RoleAxes &role,
                                                          const std::filesystem::path &output_dir) {
  const FieldSpec &spec = regsho_threshold_days63_spec();
  ATX_TRY(const auto lists, read_short_side_all<RegshoListRows>(lists_stage, "lists.parquet"));
  RegshoIndexer indexer(lists, role);
  const auto calendar = indexer.calendar();
  const std::vector<std::string> files = stage_year_files(
      lists_stage, "threshold.parquet", year_of(calendar.front()), year_of(calendar.back()));
  const RowsVisitor<ThresholdRows> add_batch = [&indexer](const ThresholdRows &rows)
      -> core::Status {
    indexer.add(rows);
    return core::Ok();
  };
  for (const std::string &rel : files) { // one row group at a time
    ATX_TRY_VOID(read_short_side<ThresholdRows>(lists_stage, rel, add_batch));
    indexer.end_file(rel);
  }
  RegshoPanel panel = indexer.finish();
  ListingIndexer listing_indexer(role);
  const RowsVisitor<FinraNameRows> add_names =
      [&listing_indexer](const FinraNameRows &rows) -> core::Status {
    listing_indexer.add(rows);
    return core::Ok();
  };
  ATX_TRY_VOID(read_short_side<FinraNameRows>(names_stage, "finra_names.parquet", add_names));
  const ListingRows listing = listing_indexer.finish();
  panel.checks["finra_name_rows_on_role"] = static_cast<u64>(listing.available_ns.size());
  ATX_TRY(auto kernel, RegshoKernel::create(panel, listing, role));
  const std::vector<i64> marks = previous_session_marks(role.days());
  ATX_TRY(auto writer, FieldWriter::create(output_dir, spec.name, role));
  std::vector<f64> row(role.instruments());
  RegshoReasons reasons;
  std::array<u64, kMarkets> by_market{};
  for (usize t = 0; t < role.dates(); ++t) {
    kernel.row(t, marks[t], role.member_row(t), reasons, by_market, row);
    ATX_TRY_VOID(writer.write(row));
  }
  ATX_TRY(auto written, writer.close());
  Json finite = Json::object();
  for (usize m = 0; m < kMarkets; ++m) {
    finite[std::string(kRegshoMarkets[m])] = by_market[m];
  }
  panel.checks["finite_member_cells_by_listing_market"] = std::move(finite);
  std::vector<SourceRecord> read = lists_stage.sources();
  const std::vector<SourceRecord> names_read = names_stage.sources();
  read.erase(read.begin());
  read.insert(read.end(), names_read.begin() + 1, names_read.end());
  HoldingsField out;
  out.field = std::move(written);
  out.sources = {lists_stage.manifest_source(), names_stage.manifest_source()};
  out.extra = Json{{"visibility_rule", std::string(kStageVisibilityRule)},
                   {"stage_manifest_sha256",
                    Json{{"regsho_threshold", lists_stage.manifest_source().sha256},
                         {"security_master", names_stage.manifest_source().sha256}}},
                   {"seal_date", std::string(data::kSealBeginDate)},
                   {"nan_reasons_member_cells",
                    Json{{"market_unknown", reasons.market_unknown},
                         {"market_lists_incomplete", reasons.market_lists_incomplete},
                         {"short_calendar", reasons.short_calendar}}}};
  out.source_checks = Json{{"visibility_rule", std::string(kStageVisibilityRule)},
                           {"seal", std::string(data::kSealBeginDate)},
                           {"window", std::string(data::kResearchWindowId)},
                           {"stage", Json{{"regsho_threshold", lists_stage.check()},
                                          {"security_master", names_stage.check()}}},
                           {"stage_files", records_json(read)},
                           {"regsho_threshold", std::move(panel.checks)}};
  return core::Ok(std::move(out));
}

core::Result<FieldPlan> parse_regsho_kind(std::string_view field, const nlohmann::json &options,
                                          const BuildSpec &spec) {
  const FieldSpec &s = regsho_threshold_days63_spec();
  ATX_TRY_VOID(check_kind_request(s, field, options));
  if (!spec.regsho_threshold || !spec.security_master) {
    return core::Err(refused(s.name + " needs regsho_threshold and security_master "
                                      "{dir, manifest_sha256}"));
  }
  FieldPlan plan;
  plan.name = s.name;
  plan.spec = &s;
  plan.options = options;
  plan.inputs = {spec.regsho_threshold->dir / "manifest.json",
                 spec.security_master->dir / "manifest.json"};
  return core::Ok(std::move(plan));
}

core::Result<KindOutput> build_regsho_kind(const FieldPlan &plan, BuildContext &ctx) {
  if (!ctx.spec.regsho_threshold || !ctx.spec.security_master) {
    return core::Err(refused(plan.name + " needs regsho_threshold and security_master"));
  }
  if (!ctx.sources.regsho_threshold) { // pinned once per run
    ATX_TRY(auto stage, PinnedStage::open(*ctx.spec.regsho_threshold, kRegshoStage));
    ctx.sources.regsho_threshold = std::move(stage);
  }
  if (!ctx.sources.security_master) {
    ATX_TRY(auto stage, PinnedStage::open(*ctx.spec.security_master, kSecurityMasterStage));
    ctx.sources.security_master = std::move(stage);
  }
  ATX_TRY(auto built, build_regsho_threshold_days63(*ctx.sources.regsho_threshold,
                                                    *ctx.sources.security_master, ctx.role,
                                                    ctx.spec.output_dir));
  KindOutput out;
  out.field = std::move(built.field);
  out.sources = std::move(built.sources);
  out.blocks = Json{{"extra", std::move(built.extra)},
                    {"source_checks", std::move(built.source_checks)}};
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
