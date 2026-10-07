// regsho_threshold_days63 (research_fields_holdings.py build_regsho; P9 lane A4). Declared in
// holdings_fields.hpp.
#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/holdings_fields.hpp"
#include "atx/engine/research/fields/session_clock.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kMarkets = kRegshoMarkets.size();
constexpr std::array<std::string_view, 2> kListStatuses{"list", "empty_list"};
// FINRA marketClassCode -> the threshold list that covers the listing (REGSHO_MARKET_OF_CLASS):
// NNM / SC -> nasdaq, NYSE / AMEX / ARCA -> nyse, BZX -> cboe_bzx, OTC -> finra_otc.
constexpr std::array<std::pair<std::string_view, i64>, 7> kMarketOfClass{{{"NNM", 0},
                                                                         {"SC", 0},
                                                                         {"NYSE", 1},
                                                                         {"AMEX", 1},
                                                                         {"ARCA", 1},
                                                                         {"BZX", 2},
                                                                         {"OTC", 3}}};

[[nodiscard]] core::Error refused(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "atx-research-fields spec: " + std::move(message));
}

[[nodiscard]] u64 members_of(std::span<const u8> member) noexcept {
  return static_cast<u64>(std::count_if(member.begin(), member.end(),
                                        [](u8 m) { return m != 0; }));
}

[[nodiscard]] bool usable_status(std::string_view status) noexcept {
  return std::find(kListStatuses.begin(), kListStatuses.end(), status) != kListStatuses.end();
}

[[nodiscard]] i64 market_of_class(std::string_view market_class) noexcept {
  const auto it = std::find_if(kMarketOfClass.begin(), kMarketOfClass.end(),
                               [market_class](const auto &c) { return c.first == market_class; });
  return it == kMarketOfClass.end() ? -1 : it->second;
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

} // namespace

RegshoIndexer::RegshoIndexer(const RegshoListRows &lists, const RoleAxes &role) : role_(&role) {
  const auto days = role.days();
  const usize count = lists.list_day.size();
  std::vector<u8> ok(count, 0);
  std::vector<i64> before;
  for (usize i = 0; i < count; ++i) {
    const bool usable =
        usable_status(lists.status[i]) && !data::is_sealed(lists.available_ns[i]);
    ok[i] = static_cast<u8>(usable ? 1 : 0);
    if (ok[i] != 0 && lists.list_day[i] < days.front()) {
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

void RegshoIndexer::add(const ThresholdRows &rows, std::string_view name) {
  names_.emplace_back(name);
  RegshoPanel &p = panel_;
  const usize dates = p.calendar.size();
  struct Use {
    usize date{};
    usize column{};
    usize market{};
    i64 available{};
    bool late{};
  };
  std::vector<Use> uses;
  for (usize i = 0; i < rows.list_day.size(); ++i) {
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
    uses.push_back(Use{*k, *column, market, rows.available_ns[i], rows.available_ns[i] > list});
  }
  for (const Use &u : uses) { // a row later than its list delays the list
    if (u.late) {
      ++late_rows_;
      i64 &slot = p.list_visible_ns[u.market * dates + u.date];
      slot = std::max(slot, u.available);
    }
  }
  for (const Use &u : uses) { // row order: the last row of a cell wins
    p.on_market[u.date * p.columns + u.column] = static_cast<u8>(u.market + 1);
    ++rows_used_;
  }
}

RegshoPanel RegshoIndexer::finish() {
  panel_.checks["threshold_files"] = Json(names_);
  panel_.checks["on_list_rows_on_role"] = rows_used_;
  panel_.checks["rows_later_than_their_list"] = late_rows_;
  return std::move(panel_);
}

ListingRows index_listing_rows(const FinraNameRows &names, const RoleAxes &role) {
  std::vector<usize> keep;
  std::vector<usize> column(names.security_id.size(), 0);
  for (usize i = 0; i < names.security_id.size(); ++i) {
    const i64 sid = names.security_id[i];
    const auto c = sid > 0 ? role.column_of(sid) : std::nullopt;
    if (c && !data::is_sealed(names.available_ns[i])) {
      keep.push_back(i);
      column[i] = *c;
    }
  }
  std::stable_sort(keep.begin(), keep.end(), [&names](usize a, usize b) {
    return names.available_ns[a] < names.available_ns[b];
  });
  ListingRows out;
  out.available_ns.reserve(keep.size());
  for (const usize i : keep) {
    out.available_ns.push_back(names.available_ns[i]);
    out.dissemination_day.push_back(names.dissemination_day[i]);
    out.column.push_back(column[i]);
    out.market.push_back(market_of_class(names.market_class[i]));
  }
  return out;
}

void regsho_row(const RegshoPanel &panel, const ListingRows &listing, std::span<const i64> latest,
                const RoleAxes &role, usize t, i64 mark, std::span<const u8> member,
                RegshoReasons &reasons, std::span<u64> counts_by_market, std::span<f64> out) {
  std::fill(out.begin(), out.end(), kNaN);
  const usize n = panel.columns;
  const usize dates = panel.calendar.size();
  const i64 day = role.days()[t];
  const i64 end = static_cast<i64>(panel.prefix + t) - 2; // the window ends at session t-2
  if (end - static_cast<i64>(kRegshoWindow) + 1 < 0) {
    reasons.short_calendar += members_of(member);
    return;
  }
  const usize last = static_cast<usize>(end);
  const usize first = last + 1 - kRegshoWindow;
  std::array<usize, kMarkets> covered{};
  for (usize m = 0; m < kMarkets; ++m) {
    for (usize w = first; w <= last; ++w) {
      covered[m] += panel.list_visible_ns[m * dates + w] < mark ? 1U : 0U;
    }
  }
  for (usize j = 0; j < n; ++j) {
    i64 market = -1; // the line's listing market from its latest visible name row
    const i64 row = latest[j];
    if (row >= 0) {
      const usize r = static_cast<usize>(row);
      if (day - listing.dissemination_day[r] <= kRegshoNameMaxAgeDays) {
        market = listing.market[r];
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
    usize count = 0;
    for (usize w = first; w <= last; ++w) {
      const u8 on = panel.on_market[w * n + j];
      if (on > 0 && panel.list_visible_ns[(on - 1U) * dates + w] < mark) {
        ++count;
      }
    }
    out[j] = static_cast<f64>(count);
    counts_by_market[mk] += member[j] != 0 ? 1U : 0U;
  }
}

core::Result<HoldingsField> build_regsho_threshold_days63(PinnedStage &lists_stage,
                                                          PinnedStage &names_stage,
                                                          const RoleAxes &role,
                                                          const std::filesystem::path &output_dir) {
  const FieldSpec &spec = regsho_threshold_days63_spec();
  ATX_TRY(const auto lists, read_regsho_lists(lists_stage));
  RegshoIndexer indexer(lists, role);
  const auto calendar = indexer.calendar();
  const std::vector<std::string> files = stage_year_files(
      lists_stage, "threshold.parquet", year_of(calendar.front()), year_of(calendar.back()));
  for (const std::string &rel : files) {
    ATX_TRY(const auto rows, read_threshold_rows(lists_stage, rel)); // one year at a time
    indexer.add(rows, rel);
  }
  RegshoPanel panel = indexer.finish();
  ATX_TRY(const auto names, read_finra_names(names_stage));
  const ListingRows listing = index_listing_rows(names, role);
  panel.checks["finra_name_rows_on_role"] = static_cast<u64>(listing.available_ns.size());
  ATX_TRY(auto latest, LatestRows::create(listing.available_ns, listing.column,
                                          role.instruments()));
  const std::vector<i64> marks = previous_session_marks(role.days());
  ATX_TRY(auto writer, FieldWriter::create(output_dir, spec.name, role));
  std::vector<f64> row(role.instruments());
  RegshoReasons reasons;
  std::array<u64, kMarkets> by_market{};
  for (usize t = 0; t < role.dates(); ++t) {
    const auto visible = latest.before(marks[t]); // name rows with available_at < mark(t-1)
    regsho_row(panel, listing, visible, role, t, marks[t], role.member_row(t), reasons,
               by_market, row);
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
