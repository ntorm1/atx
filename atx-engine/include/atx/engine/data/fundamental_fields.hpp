#pragma once

// atx::engine::data::fundamentals — point-in-time fundamental DSL fields (lane 10).
//
// Turns filing-level accounting snapshots into date x instrument panels that the
// alpha DSL loads through the same `LoadField` path as `close`, then derives the
// literature ratios a fundamental alpha zoo needs.
//
// Division of labour (why the header stops where it does)
// -------------------------------------------------------
// Fiscal-period arithmetic (trailing-twelve-month sums, year-ago balances,
// standardized unexpected earnings over quarterly history) needs the full
// statement history of a company and is done once, per filing, by the export
// tool `atx-engine/tools/export_fundamental_fields.py`. Each output row is a
// `PitRecord`: what an investor could know about one company at the instant a
// filing became public. This header owns everything that depends on the panel
// axis — the as-of join, the lag, the forward fill with staleness caps, the
// security-id mapping and every ratio whose denominator is a daily price.
//
// Point-in-time rule (the central correctness requirement)
// ---------------------------------------------------------
// A record is keyed by `available_at` — the first instant the filing is public
// under the warehouse clock policy (`sec_filed_date_plus_46h_v1`), NOT by its
// fiscal period end. It becomes visible on the first session whose key is at or
// after `available_at`, plus `lag_sessions` further sessions. Among the records
// visible at a session, the one describing the LATEST fiscal period wins; ties
// on period go to the later `available_at` (a restatement of the same period
// replaces it, a late amendment of an older period never regresses a newer
// figure). Selection is per field, so a filing that omits a line item keeps the
// previous filing's value for that item. A chosen value is dropped (NaN) once it
// is older than either staleness cap, so a company that stops filing leaves the
// cross-section instead of carrying a frozen balance sheet forward.
//
// Header-only; construction is a COLD path (once per panel window), so the
// std::vector allocations here are fine. Errors travel in Result; nothing
// throws on a success path.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atx::engine::data::fundamentals {

inline constexpr atx::i64 kNanosPerDay = 86'400'000'000'000;

// Raw accounting quantities exported per filing (units: USD, shares, or a
// standardized score for `Sue`). The order is the column order of the export
// tool's `points.csv` after its fixed key columns; renumbering breaks readers.
enum class RawField : atx::u8 {
  BookEquity = 0,       // StockholdersEquity at the latest balance-sheet date
  TotalAssets,          // Assets at the latest balance-sheet date
  TotalLiabilities,     // Liabilities (or Assets - equity incl. NCI)
  AssetsLag1y,          // Assets at the balance-sheet date ~1 year earlier
  NetIncomeTtm,         // NetIncomeLoss, trailing four quarters
  RevenueTtm,           // Revenues (and ASC 606 / legacy synonyms), TTM
  GrossProfitTtm,       // GrossProfit, or revenue - cost of revenue, TTM
  OperatingCashFlowTtm, // NetCashProvidedByUsedInOperatingActivities, TTM
  OperatingIncomeTtm,   // OperatingIncomeLoss, TTM
  SharesOutstanding,    // weighted-average diluted shares, latest discrete period
  SharesLag1y,          // the same ~1 year earlier, as restated (split-consistent)
  Sue,                  // standardized unexpected quarterly net income, seasonal RW
  Count
};
inline constexpr atx::usize kRawFieldCount = static_cast<atx::usize>(RawField::Count);

inline constexpr std::array<std::string_view, kRawFieldCount> kRawFieldNames = {
    "book_equity",        "total_assets",   "total_liabilities", "assets_lag1y",
    "net_income_ttm",     "revenue_ttm",    "gross_profit_ttm",  "operating_cash_flow_ttm",
    "operating_income_ttm", "shares_outstanding", "shares_lag1y", "sue"};

// Ratios derived on the panel axis. Citations are for the definition used.
enum class DerivedField : atx::u8 {
  BookToPrice = 0,        // book equity / market cap (Fama-French 1992); book <= 0 -> NaN
  EarningsYield,          // NI_ttm / market cap (Basu 1977)
  SalesYield,             // revenue_ttm / market cap (Barbee-Mukherji-Raines 1996)
  CashflowYield,          // CFO_ttm / market cap (Lakonishok-Shleifer-Vishny 1994)
  Roe,                    // NI_ttm / book equity; book <= 0 -> NaN
  Roa,                    // NI_ttm / assets
  GrossProfitability,     // gross profit_ttm / assets (Novy-Marx 2013)
  OperatingProfitability, // operating income_ttm / book equity (Fama-French 2015 RMW)
  Accruals,               // (NI_ttm - CFO_ttm) / mean(assets, assets_lag1y)
                          // (Sloan 1996, cash-flow form of Hribar-Collins 2002)
  AssetGrowth,            // assets / assets_lag1y - 1 (Cooper-Gulen-Schill 2008)
  NetIssuance,            // log(shares / shares_lag1y) (Pontiff-Woodgate 2008)
  Leverage,               // liabilities / assets
  Sue,                    // passthrough of the raw SUE score (Bernard-Thomas 1989)
  Count
};
inline constexpr atx::usize kDerivedFieldCount = static_cast<atx::usize>(DerivedField::Count);

inline constexpr std::array<std::string_view, kDerivedFieldCount> kDerivedFieldNames = {
    "book_to_price", "earnings_yield", "sales_yield",  "cashflow_yield", "roe",
    "roa",           "gross_profitability", "operating_profitability", "accruals",
    "asset_growth",  "net_issuance",   "leverage",     "sue_score"};

[[nodiscard]] inline std::optional<RawField> raw_field_from_name(std::string_view name) noexcept {
  for (atx::usize f = 0; f < kRawFieldCount; ++f) {
    if (kRawFieldNames[f] == name) return static_cast<RawField>(f);
  }
  return std::nullopt;
}

// One filing-level snapshot for one panel instrument. NaN = not reported.
struct PitRecord {
  atx::usize instrument{};  // panel column (see map_security_ids)
  atx::i64 available_ns{};  // first public instant, Unix ns
  atx::i64 period_end_ns{}; // fiscal period end the values describe, Unix ns
  std::array<atx::f64, kRawFieldCount> values{};
};

struct AlignConfig {
  atx::usize lag_sessions{1};              // extra sessions after first visibility
  atx::i64 max_days_since_available{400};  // > 0; forward-fill cap from filing
  atx::i64 max_days_since_period_end{550}; // > 0; cap from fiscal period end
};

struct AlignStats {
  atx::usize records{};            // records supplied
  atx::usize records_after_axis{}; // visible only after the last session: never placed
  std::array<atx::usize, kRawFieldCount> filled_cells{}; // finite cells written
  std::array<atx::usize, kRawFieldCount> stale_cells{};  // visible value dropped by a cap
};

struct AlignedFundamentals {
  atx::usize dates{};
  atx::usize instruments{};
  std::array<std::vector<atx::f64>, kRawFieldCount> raw; // date-major, dates*instruments
  AlignStats stats;
};

namespace detail {

[[nodiscard]] inline atx::core::Result<bool>
validate_axis(std::span<const atx::i64> session_keys, atx::usize instruments) {
  if (instruments == 0 || session_keys.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fundamentals: empty panel axis");
  }
  for (atx::usize t = 1; t < session_keys.size(); ++t) {
    if (session_keys[t] <= session_keys[t - 1]) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "fundamentals: session keys not strictly increasing");
    }
  }
  return atx::core::Ok(true);
}

// Session index at which a record becomes visible; `session_keys.size()` means
// never within this axis.
[[nodiscard]] inline atx::usize visible_index(std::span<const atx::i64> session_keys,
                                              atx::i64 available_ns, atx::usize lag) noexcept {
  const auto it = std::lower_bound(session_keys.begin(), session_keys.end(), available_ns);
  const auto first = static_cast<atx::usize>(it - session_keys.begin());
  const atx::usize n = session_keys.size();
  return (first >= n || lag >= n - first) ? n : first + lag;
}

// True when candidate `b` should replace the current choice `a`.
[[nodiscard]] inline bool supersedes(const PitRecord &b, const PitRecord &a) noexcept {
  if (b.period_end_ns != a.period_end_ns) return b.period_end_ns > a.period_end_ns;
  return b.available_ns >= a.available_ns;
}

} // namespace detail

// As-of join of filing snapshots onto a session axis (see the header comment).
//
// Errors: InvalidArgument on an empty or non-increasing axis, a non-positive
// staleness cap, an instrument outside [0, instruments), or a record whose
// period ends after it became public (a stamped-in-the-future fact is a clock
// bug upstream, never data). Output columns are sized dates*instruments.
[[nodiscard]] inline atx::core::Result<AlignedFundamentals>
align_pit_records(std::span<const PitRecord> records, std::span<const atx::i64> session_keys,
                  atx::usize instruments, const AlignConfig &cfg) {
  ATX_TRY(auto axis_ok, detail::validate_axis(session_keys, instruments));
  static_cast<void>(axis_ok);
  if (cfg.max_days_since_available <= 0 || cfg.max_days_since_period_end <= 0) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fundamentals: staleness caps must be positive");
  }
  const atx::usize dates = session_keys.size();
  AlignedFundamentals out{};
  out.dates = dates;
  out.instruments = instruments;
  out.stats.records = records.size();
  for (auto &col : out.raw) col.assign(dates * instruments, std::numeric_limits<atx::f64>::quiet_NaN());

  // Bucket record indices per instrument, ordered by visibility.
  std::vector<std::vector<std::pair<atx::usize, atx::usize>>> by_inst(instruments);
  for (atx::usize r = 0; r < records.size(); ++r) {
    const PitRecord &rec = records[r];
    if (rec.instrument >= instruments) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "fundamentals: record instrument outside the panel axis");
    }
    if (rec.period_end_ns > rec.available_ns) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "fundamentals: record period ends after it became public");
    }
    const atx::usize vis = detail::visible_index(session_keys, rec.available_ns, cfg.lag_sessions);
    if (vis >= dates) {
      ++out.stats.records_after_axis;
      continue;
    }
    by_inst[rec.instrument].emplace_back(vis, r);
  }

  const atx::i64 cap_avail = cfg.max_days_since_available * kNanosPerDay;
  const atx::i64 cap_period = cfg.max_days_since_period_end * kNanosPerDay;
  for (atx::usize i = 0; i < instruments; ++i) {
    auto &list = by_inst[i];
    std::sort(list.begin(), list.end());
    for (atx::usize f = 0; f < kRawFieldCount; ++f) {
      const PitRecord *best = nullptr;
      atx::usize next = 0;
      for (atx::usize t = 0; t < dates; ++t) {
        for (; next < list.size() && list[next].first <= t; ++next) {
          const PitRecord &cand = records[list[next].second];
          if (!std::isfinite(cand.values[f])) continue;
          if (best == nullptr || detail::supersedes(cand, *best)) best = &cand;
        }
        if (best == nullptr) continue;
        const atx::i64 key = session_keys[t];
        if (key - best->available_ns > cap_avail || key - best->period_end_ns > cap_period) {
          ++out.stats.stale_cells[f];
          continue;
        }
        out.raw[f][t * instruments + i] = best->values[f];
        ++out.stats.filled_cells[f];
      }
    }
  }
  return atx::core::Ok(std::move(out));
}

// Map external security ids onto panel columns. `axis_ids[c]` is the id of panel
// column c; the result holds, per `record_ids[k]`, its column or nullopt when
// the id is not on the axis (an unmapped record is dropped by the caller, never
// placed on a guessed column). Errors: InvalidArgument on a duplicated axis id.
[[nodiscard]] inline atx::core::Result<std::vector<std::optional<atx::usize>>>
map_security_ids(std::span<const std::string> axis_ids, std::span<const std::string> record_ids) {
  std::unordered_map<std::string_view, atx::usize> column;
  column.reserve(axis_ids.size());
  for (atx::usize c = 0; c < axis_ids.size(); ++c) {
    if (!column.emplace(axis_ids[c], c).second) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "fundamentals: duplicate security id on the panel axis");
    }
  }
  std::vector<std::optional<atx::usize>> out(record_ids.size());
  for (atx::usize k = 0; k < record_ids.size(); ++k) {
    const auto it = column.find(record_ids[k]);
    if (it != column.end()) out[k] = it->second;
  }
  return atx::core::Ok(std::move(out));
}

namespace detail {

[[nodiscard]] inline atx::f64 ratio_pos(atx::f64 num, atx::f64 den) noexcept {
  if (!std::isfinite(num) || !std::isfinite(den) || den <= 0.0) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  return num / den;
}

} // namespace detail

// Derive the literature ratios (DerivedField) cell by cell. `market_cap` is the
// panel's date-major market capitalization in the same currency unit as the
// raw fields. Every ratio is NaN when an input is missing or a denominator is
// non-positive. Errors: InvalidArgument when `market_cap` or any raw column is
// not sized dates*instruments.
[[nodiscard]] inline atx::core::Result<std::array<std::vector<atx::f64>, kDerivedFieldCount>>
derive_fields(const AlignedFundamentals &a, std::span<const atx::f64> market_cap) {
  const atx::usize cells = a.dates * a.instruments;
  if (market_cap.size() != cells) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fundamentals: market_cap extent differs from the panel");
  }
  for (const auto &col : a.raw) {
    if (col.size() != cells) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "fundamentals: raw field extent differs from the panel");
    }
  }
  constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
  std::array<std::vector<atx::f64>, kDerivedFieldCount> out;
  for (auto &col : out) col.assign(cells, kNaN);
  auto raw = [&](RawField f, atx::usize c) { return a.raw[static_cast<atx::usize>(f)][c]; };
  auto put = [&](DerivedField f, atx::usize c, atx::f64 v) {
    out[static_cast<atx::usize>(f)][c] = std::isfinite(v) ? v : kNaN;
  };
  using detail::ratio_pos;
  for (atx::usize c = 0; c < cells; ++c) {
    const atx::f64 mcap = market_cap[c];
    const atx::f64 book = raw(RawField::BookEquity, c);
    const atx::f64 assets = raw(RawField::TotalAssets, c);
    const atx::f64 assets_ly = raw(RawField::AssetsLag1y, c);
    const atx::f64 ni = raw(RawField::NetIncomeTtm, c);
    const atx::f64 cfo = raw(RawField::OperatingCashFlowTtm, c);
    put(DerivedField::BookToPrice, c, book > 0.0 ? ratio_pos(book, mcap) : kNaN);
    put(DerivedField::EarningsYield, c, ratio_pos(ni, mcap));
    put(DerivedField::SalesYield, c, ratio_pos(raw(RawField::RevenueTtm, c), mcap));
    put(DerivedField::CashflowYield, c, ratio_pos(cfo, mcap));
    put(DerivedField::Roe, c, ratio_pos(ni, book));
    put(DerivedField::Roa, c, ratio_pos(ni, assets));
    put(DerivedField::GrossProfitability, c, ratio_pos(raw(RawField::GrossProfitTtm, c), assets));
    put(DerivedField::OperatingProfitability, c,
        ratio_pos(raw(RawField::OperatingIncomeTtm, c), book));
    const atx::f64 avg_assets =
        (assets > 0.0 && assets_ly > 0.0) ? 0.5 * (assets + assets_ly) : kNaN;
    put(DerivedField::Accruals, c, ratio_pos(ni - cfo, avg_assets));
    put(DerivedField::AssetGrowth, c,
        assets > 0.0 ? ratio_pos(assets, assets_ly) - 1.0 : kNaN);
    const atx::f64 shares = raw(RawField::SharesOutstanding, c);
    const atx::f64 share_ratio = shares > 0.0 ? ratio_pos(shares, raw(RawField::SharesLag1y, c))
                                              : kNaN;
    put(DerivedField::NetIssuance, c, share_ratio > 0.0 ? std::log(share_ratio) : kNaN);
    put(DerivedField::Leverage, c, ratio_pos(raw(RawField::TotalLiabilities, c), assets));
    put(DerivedField::Sue, c, raw(RawField::Sue, c));
  }
  return atx::core::Ok(std::move(out));
}

// A new owned Panel = `base` + every raw column (kRawFieldNames) + every derived
// column (kDerivedFieldNames), universe mask unchanged, so a DSL expression can
// reference e.g. `rank(book_to_price)`. Errors: InvalidArgument on a shape
// mismatch or when an appended name already exists in `base` (a collision is
// refused rather than shadowing a field the caller may already rely on).
[[nodiscard]] inline atx::core::Result<atx::engine::alpha::Panel>
with_fundamental_fields(const atx::engine::alpha::Panel &base, const AlignedFundamentals &a,
                        const std::array<std::vector<atx::f64>, kDerivedFieldCount> &derived) {
  const atx::usize dates = base.dates();
  const atx::usize insts = base.instruments();
  if (a.dates != dates || a.instruments != insts) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fundamentals: aligned shape differs from the base panel");
  }
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  const atx::usize total = base.num_fields() + kRawFieldCount + kDerivedFieldCount;
  names.reserve(total);
  data.reserve(total);
  for (atx::usize f = 0; f < base.num_fields(); ++f) {
    names.push_back(base.field_name(f));
    const auto col = base.field_all(static_cast<atx::engine::alpha::FieldId>(f));
    data.emplace_back(col.begin(), col.end());
  }
  auto append = [&](std::string_view name, const std::vector<atx::f64> &col) -> bool {
    if (base.field_id(name).has_value() || col.size() != dates * insts) return false;
    names.emplace_back(name);
    data.push_back(col);
    return true;
  };
  for (atx::usize f = 0; f < kRawFieldCount; ++f) {
    if (!append(kRawFieldNames[f], a.raw[f])) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "fundamentals: raw field collides with the base panel or is ragged");
    }
  }
  for (atx::usize f = 0; f < kDerivedFieldCount; ++f) {
    if (!append(kDerivedFieldNames[f], derived[f])) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "fundamentals: derived field collides with the base panel or is ragged");
    }
  }
  std::vector<std::uint8_t> mask(dates * insts);
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize i = 0; i < insts; ++i) mask[d * insts + i] = base.in_universe(d, i) ? 1U : 0U;
  }
  return atx::engine::alpha::Panel::create(dates, insts, std::move(names), std::move(data),
                                           std::move(mask));
}

} // namespace atx::engine::data::fundamentals
