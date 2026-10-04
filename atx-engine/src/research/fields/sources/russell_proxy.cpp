#include "atx/engine/research/fields/sources/russell_proxy.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

void sort_unique(std::vector<i64> &ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

// The reconstitution of `day` in `recons` (ascending by recon_day), appended when absent.
[[nodiscard]] RussellRecon &recon_of(std::vector<RussellRecon> &recons, i64 day) {
  const auto it = std::lower_bound(recons.begin(), recons.end(), day,
                                   [](const RussellRecon &r, i64 d) { return r.recon_day < d; });
  if (it != recons.end() && it->recon_day == day) {
    return *it;
  }
  RussellRecon fresh;
  fresh.recon_day = day;
  fresh.rank_day = kNullDay;
  fresh.visible_ns = kBeforeAllNs;
  return *recons.insert(it, std::move(fresh));
}

} // namespace

core::Result<RussellSnapshots> russell_snapshots(std::span<const RussellRow> rows) {
  RussellSnapshots out;
  for (const RussellRow &r : rows) {
    ++out.counts.rows_total;
    const auto seen = std::lower_bound(out.index_ids.begin(), out.index_ids.end(), r.index_id);
    if (seen == out.index_ids.end() || *seen != r.index_id) {
      out.index_ids.insert(seen, r.index_id);
    }
    const bool r1000 = r.index_id == kRussell1000Id;
    if (!r1000 && r.index_id != kRussell2000Id) {
      ++out.counts.rows_other_index;
      continue;
    }
    if (r.security_id < 0 || r.recon_day == kNullDay || r.available_ns == kNeverNs) {
      ++out.counts.rows_unusable;
      continue;
    }
    if (r.available_ns >= data::kSealBeginNs) { // reader-side seal: dropped before use, counted
      ++out.counts.rows_sealed;
      continue;
    }
    RussellRecon &recon = recon_of(out.recons, r.recon_day);
    recon.rank_day = std::max(recon.rank_day, r.rank_day);
    recon.visible_ns = std::max(recon.visible_ns, r.available_ns);
    (r1000 ? recon.r1000 : recon.r2000).push_back(r.security_id);
  }
  const u64 index_rows =
      out.counts.rows_total - out.counts.rows_other_index - out.counts.rows_unusable;
  if (index_rows == 0) { // the declared ids name no usable row: refuse, listing the ids seen
    std::string ids;
    for (const std::string &id : out.index_ids) {
      ids += (ids.empty() ? "" : ", ") + id;
    }
    return core::Err(core::ErrorCode::InvalidArgument,
                     "indexes stage: no usable " + std::string(kRussell1000Id) + " / " +
                         std::string(kRussell2000Id) + " row (index ids seen: " +
                         (ids.empty() ? std::string("none") : ids) + ")");
  }
  for (RussellRecon &recon : out.recons) {
    sort_unique(recon.r1000);
    sort_unique(recon.r2000);
  }
  return core::Ok(std::move(out));
}

core::Result<RussellSnapshots> read_russell_snapshots(PinnedStage &stage) {
  constexpr std::array<std::string_view, 5> kColumns{"index_id", "security_id", "rank_date",
                                                     "recon_date", "available_at"};
  ATX_TRY(const auto table, stage.table(kConstituentsFile, kColumns));
  ATX_TRY(const auto index_id, text_column(table, "index_id"));
  ATX_TRY(const auto security_id, id_column(table, "security_id"));
  ATX_TRY(const auto rank_day, day_column(table, "rank_date"));
  ATX_TRY(const auto recon_day, day_column(table, "recon_date"));
  ATX_TRY(const auto available, instant_column(table, "available_at"));
  std::vector<RussellRow> rows;
  rows.reserve(index_id.size());
  for (usize i = 0; i < index_id.size(); ++i) {
    rows.push_back(
        RussellRow{index_id[i], security_id[i], rank_day[i], recon_day[i], available[i]});
  }
  return russell_snapshots(rows);
}

nlohmann::json russell_checks(const RussellSnapshots &snapshots) {
  Json recons = Json::array();
  for (const RussellRecon &r : snapshots.recons) {
    recons.push_back(Json{{"recon_date", iso_date(r.recon_day)},
                          {"rank_date", r.rank_day == kNullDay ? Json(nullptr)
                                                               : Json(iso_date(r.rank_day))},
                          {"visible_at", iso_instant(r.visible_ns)},
                          {"r1000_lines", r.r1000.size()},
                          {"r2000_lines", r.r2000.size()}});
  }
  const RussellCounts &c = snapshots.counts;
  return Json{{"index_ids_seen", snapshots.index_ids},
              {"index_ids_used",
               Json::array({std::string(kRussell1000Id), std::string(kRussell2000Id)})},
              {"rows_total", c.rows_total},
              {"rows_other_index", c.rows_other_index},
              {"rows_sealed_dropped", c.rows_sealed},
              {"rows_unusable", c.rows_unusable},
              {"reconstitutions", std::move(recons)}};
}

} // namespace atx::engine::research::fields
