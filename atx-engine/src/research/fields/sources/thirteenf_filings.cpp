// The 13F stage reader, filings half: quarter ends, the effective-filing rule, quarter visibility,
// unit factors and CUSIP maps (research_fields_holdings.py / research_fields_mgr13f.py). Declared
// in thirteenf.hpp.
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <optional>
#include <tuple>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/sources/parquet_columns.hpp"
#include "atx/engine/research/fields/sources/thirteenf.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kBaseForm = "13F-HR";
constexpr std::array<std::string_view, 2> kForms{"13F-HR", "13F-HR/A"};
constexpr std::string_view kNewHoldings = "NEW HOLDINGS";
constexpr std::string_view kRestatement = "RESTATEMENT";

constexpr std::string_view kEffectiveRule =
    "13f-asof45-effective-v1: per (filer CIK, period_q) among 13F-HR / 13F-HR/A filings with "
    "filing_date <= the stage deadline (first SEC business day >= period + 45 d): base = the "
    "latest (filing_date, accession) 13F-HR or amendment_type RESTATEMENT; plus every NEW HOLDINGS "
    "amendment after the base; a 13F-HR/A without an amendment type and every filing after the "
    "deadline are not used (counted)";

[[nodiscard]] bool is_form(std::string_view subtype) noexcept {
  return std::find(kForms.begin(), kForms.end(), subtype) != kForms.end();
}

// The days of quarter ends (ascending) holding `day`, or nullopt.
[[nodiscard]] std::optional<usize> quarter_index(std::span<const i64> quarter_days, i64 day) {
  const auto it = std::lower_bound(quarter_days.begin(), quarter_days.end(), day);
  if (it == quarter_days.end() || *it != day) {
    return std::nullopt;
  }
  return static_cast<usize>(it - quarter_days.begin());
}

// First-row-wins unique of (key, value) pairs, ascending by key.
template <class V>
[[nodiscard]] std::vector<std::pair<std::string, V>>
first_rows(std::vector<std::pair<std::string, V>> rows) {
  std::stable_sort(rows.begin(), rows.end(),
                   [](const auto &a, const auto &b) { return a.first < b.first; });
  const auto last = std::unique(rows.begin(), rows.end(),
                                [](const auto &a, const auto &b) { return a.first == b.first; });
  rows.erase(last, rows.end());
  return rows;
}

// The effective filings of each quarter (indices into `filings`), deadline and form filtered.
void assign_effective(const std::vector<ThirteenfFiling> &filings,
                      std::span<const i64> quarter_days, std::vector<ThirteenfQuarter> &quarters,
                      ThirteenfCounts &counts) {
  std::vector<usize> selected;
  std::vector<ThirteenfFiling> rows;
  for (usize i = 0; i < filings.size(); ++i) {
    const ThirteenfFiling &f = filings[i];
    if (f.filing_day <= f.deadline_day && is_form(f.submission_type)) {
      selected.push_back(i);
      rows.push_back(f);
    }
  }
  const std::vector<u8> effective = effective_filings(rows);
  for (usize k = 0; k < selected.size(); ++k) {
    if (effective[k] == 0) {
      continue;
    }
    const auto q = quarter_index(quarter_days, filings[selected[k]].period_day);
    if (q) {
      quarters[*q].effective.push_back(selected[k]);
      ++counts.effective_filings;
    }
  }
}

} // namespace

f64 ThirteenfIndex::unit_factor_of(std::string_view accession) const noexcept {
  const auto it = std::lower_bound(
      unit_factor.begin(), unit_factor.end(), accession,
      [](const std::pair<std::string, f64> &a, std::string_view key) {
        return std::string_view(a.first) < key;
      });
  if (it == unit_factor.end() || std::string_view(it->first) != accession) {
    return 1.0;
  }
  const f64 factor = it->second;
  return std::isfinite(factor) && factor > 0.0 ? factor : 1.0;
}

i64 quarter_end_on_or_after(i64 day) noexcept {
  const CivilDate d = civil_from_days(day);
  const u32 month = ((d.month - 1U) / 3U) * 3U + 3U;
  const i64 next_year = d.year + (month == 12U ? 1 : 0);
  const u32 next_month = month % 12U + 1U;
  return days_from_civil(next_year, next_month, 1U) - 1;
}

i64 previous_quarter_end(i64 quarter_end) noexcept {
  const CivilDate d = civil_from_days(quarter_end);
  return quarter_end_on_or_after(days_from_civil(d.year, d.month, 1U) - 80);
}

i64 next_quarter_end(i64 quarter_end) noexcept { return quarter_end_on_or_after(quarter_end + 1); }

std::vector<i64> thirteenf_quarters(i64 first_day, i64 last_day, usize history) {
  const i64 q0 = quarter_end_on_or_after(first_day - kThirteenfStaleDays);
  std::vector<i64> out;
  i64 earliest = q0;
  for (usize k = 0; k < history; ++k) {
    earliest = previous_quarter_end(earliest);
  }
  for (i64 q = earliest; q < q0; q = next_quarter_end(q)) {
    out.push_back(q);
  }
  for (i64 q = q0; q < last_day; q = next_quarter_end(q)) {
    out.push_back(q);
  }
  return out;
}

bool thirteenf_part_is_sealed(std::string_view part) noexcept {
  if (part.size() != 6 || part[4] != 'q' || part[5] < '1' || part[5] > '4') {
    return false;
  }
  i64 year = 0;
  for (usize i = 0; i < 4; ++i) {
    if (part[i] < '0' || part[i] > '9') {
      return false;
    }
    year = year * 10 + static_cast<i64>(part[i] - '0');
  }
  const auto quarter = static_cast<u32>(part[5] - '0');
  return is_sealed_day(days_from_civil(year, (quarter - 1U) * 3U + 1U, 1U));
}

std::vector<u8> effective_filings(std::span<const ThirteenfFiling> rows) {
  const usize n = rows.size();
  std::vector<u8> out(n, 0);
  std::vector<usize> order(n);
  std::iota(order.begin(), order.end(), usize{0});
  std::stable_sort(order.begin(), order.end(), [&rows](usize a, usize b) {
    const ThirteenfFiling &x = rows[a];
    const ThirteenfFiling &y = rows[b];
    return std::tie(x.period_day, x.filer, x.filing_day, x.accession) <
           std::tie(y.period_day, y.filer, y.filing_day, y.accession);
  });
  usize start = 0;
  while (start < n) { // one (period, filer) group per pass
    usize end = start + 1;
    while (end < n && rows[order[end]].period_day == rows[order[start]].period_day &&
           rows[order[end]].filer == rows[order[start]].filer) {
      ++end;
    }
    std::optional<usize> base;
    for (usize k = start; k < end; ++k) {
      const ThirteenfFiling &f = rows[order[k]];
      const bool is_base = f.amendment_type != kNewHoldings &&
                           (f.submission_type == kBaseForm || f.amendment_type == kRestatement);
      if (is_base) {
        base = k;
      }
    }
    if (base) {
      out[order[*base]] = 1;
      for (usize k = *base + 1; k < end; ++k) {
        if (rows[order[k]].amendment_type == kNewHoldings) {
          out[order[k]] = 1;
        }
      }
    }
    start = end;
  }
  return out;
}

ThirteenfIndex index_thirteenf(std::span<const i64> quarter_days,
                               std::vector<ThirteenfFiling> filings,
                               std::span<const UnitFactorRow> factors,
                               std::span<const CusipMapRow> cusips) {
  ThirteenfIndex out;
  filings.erase(std::remove_if(filings.begin(), filings.end(),
                               [quarter_days](const ThirteenfFiling &f) {
                                 return !quarter_index(quarter_days, f.period_day).has_value();
                               }),
                filings.end());
  out.filings = std::move(filings);
  out.quarters.resize(quarter_days.size());
  std::vector<i64> latest(quarter_days.size(), kBeforeAllNs);
  for (usize q = 0; q < quarter_days.size(); ++q) {
    out.quarters[q].period_day = quarter_days[q];
  }
  for (const ThirteenfFiling &f : out.filings) {
    if (f.filing_day > f.deadline_day) {
      ++out.counts.filings_after_deadline_ignored;
      continue;
    }
    const usize q = *quarter_index(quarter_days, f.period_day);
    out.quarters[q].has_filing = true;
    latest[q] = std::max(latest[q], f.available_ns);
  }
  for (usize q = 0; q < quarter_days.size(); ++q) {
    ThirteenfQuarter &quarter = out.quarters[q];
    if (!quarter.has_filing) {
      continue;
    }
    quarter.sealed = latest[q] >= data::kSealBeginNs;
    if (quarter.sealed) {
      ++out.counts.quarters_sealed;
    } else {
      quarter.visible_ns = latest[q];
    }
  }
  assign_effective(out.filings, quarter_days, out.quarters, out.counts);
  std::vector<std::pair<std::string, f64>> unit;
  unit.reserve(factors.size());
  for (const UnitFactorRow &r : factors) {
    unit.emplace_back(r.accession, r.factor);
  }
  out.unit_factor = first_rows(std::move(unit));
  std::vector<std::vector<std::pair<std::string, i64>>> maps(quarter_days.size());
  for (const CusipMapRow &r : cusips) {
    // A null map CUSIP (decoded "") maps nothing. Stricter than house build_13f, whose
    // pc.index_in (skip_nulls=False) would match a null holding CUSIP to a null map row.
    if (r.cusip.empty()) {
      continue;
    }
    if (const auto q = quarter_index(quarter_days, r.period_day)) {
      maps[*q].emplace_back(r.cusip, r.security_id);
    }
  }
  for (usize q = 0; q < quarter_days.size(); ++q) {
    out.quarters[q].cusip_sid = first_rows(std::move(maps[q]));
  }
  return out;
}

core::Result<ThirteenfIndex> read_thirteenf_index(PinnedStage &stage,
                                                  std::span<const i64> quarter_days) {
  std::vector<ThirteenfFiling> filings;
  constexpr std::array<std::string_view, 9> kFilingColumns{
      "accession",      "filer_cik",    "period_q",  "filing_date",  "submission_type",
      "amendment_type", "available_at", "deadline", "source_period"};
  ATX_TRY_VOID(stage.for_each_group(
      "filings.parquet", kFilingColumns,
      [&filings, quarter_days](const core::io::ParquetTable &t) -> core::Status {
        ATX_TRY(const auto period, day_column(t, "period_q"));
        ATX_TRY(const auto accession, text_column(t, "accession"));
        ATX_TRY(const auto filer, id_column(t, "filer_cik"));
        ATX_TRY(const auto filed, day_column(t, "filing_date"));
        ATX_TRY(const auto subtype, text_column(t, "submission_type"));
        ATX_TRY(const auto amend, text_column(t, "amendment_type"));
        ATX_TRY(const auto available, instant_column(t, "available_at"));
        ATX_TRY(const auto deadline, day_column(t, "deadline"));
        ATX_TRY(const auto part, text_column(t, "source_period"));
        for (usize i = 0; i < period.size(); ++i) {
          if (!quarter_index(quarter_days, period[i])) {
            continue;
          }
          filings.push_back(ThirteenfFiling{accession[i], subtype[i], amend[i], part[i], filer[i],
                                            period[i], filed[i], deadline[i], available[i]});
        }
        return core::Ok();
      }));
  constexpr std::array<std::string_view, 2> kCheckColumns{"accession", "unit_factor"};
  ATX_TRY(const auto checks, stage.table("filing_checks.parquet", kCheckColumns));
  ATX_TRY(const auto check_accession, text_column(checks, "accession"));
  ATX_TRY(const auto check_factor, real_column(checks, "unit_factor"));
  std::vector<UnitFactorRow> factors;
  factors.reserve(check_accession.size());
  for (usize i = 0; i < check_accession.size(); ++i) {
    factors.push_back(UnitFactorRow{check_accession[i], check_factor[i]});
  }
  std::vector<CusipMapRow> cusips;
  constexpr std::array<std::string_view, 3> kMapColumns{"period_q", "cusip", "security_id"};
  ATX_TRY_VOID(stage.for_each_group(
      "cusip_map_pit.parquet", kMapColumns,
      [&cusips, quarter_days](const core::io::ParquetTable &t) -> core::Status {
        ATX_TRY(const auto period, day_column(t, "period_q"));
        ATX_TRY(const auto cusip, text_column(t, "cusip"));
        ATX_TRY(const auto sid, id_column(t, "security_id"));
        for (usize i = 0; i < period.size(); ++i) {
          if (quarter_index(quarter_days, period[i])) {
            cusips.push_back(CusipMapRow{period[i], cusip[i], sid[i]});
          }
        }
        return core::Ok();
      }));
  return core::Ok(index_thirteenf(quarter_days, std::move(filings), factors, cusips));
}

nlohmann::json thirteenf_checks(const ThirteenfIndex &index) {
  Json visible = Json::object();
  Json quarters = Json::array();
  for (const ThirteenfQuarter &q : index.quarters) {
    const std::string day = iso_date(q.period_day);
    quarters.push_back(day);
    visible[day] = q.visible_ns == kNeverNs ? Json(nullptr) : Json(iso_instant(q.visible_ns));
  }
  const ThirteenfCounts &c = index.counts;
  return Json{{"quarters", std::move(quarters)},
              {"effective_rule", std::string(kEffectiveRule)},
              {"effective_filings", c.effective_filings},
              {"filings_after_deadline_ignored", c.filings_after_deadline_ignored},
              {"quarters_sealed", c.quarters_sealed},
              {"holdings_parts_not_read_sealed", c.holdings_parts_not_read_sealed},
              {"holdings_rows_kept", c.holdings_rows_kept},
              {"quarter_visible_at", std::move(visible)}};
}

} // namespace atx::engine::research::fields
