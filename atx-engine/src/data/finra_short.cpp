// atx::engine::data — FINRA consolidated short-interest loader (Track B1).
//
// Reads the hive-partitioned FINRA parquet (date=YYYY-MM-DD/part-*.parquet) and
// projects three derived, CAUSALLY placed feature columns onto an externally
// supplied research-panel axis. See finra_short.hpp for the contract; the
// causality model is: a (symbol, settlement_day) observation becomes visible on
// panel dates >= finra_first_usable_day(settlement_day, lag) (default: the 8th NYSE
// session after settlement = 7th-business-day release + 1 for the after-close
// publication) and is forward-filled until the next observation becomes visible.
//
// Parquet is read only through atx::core::io::read_parquet (PIMPL; no Arrow
// headers here), mirroring atx-tsdb/src/load_parquet.cpp.

#include "atx/engine/data/finra_short.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <filesystem>

#include "atx/core/datetime.hpp" // Date, Calendar (NYSE rule holidays)
#include "atx/core/error.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::data {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

namespace {

namespace fs = std::filesystem;

// Nanoseconds per UTC day (matches atx::core::time::Duration::kNsPerDay). Used to
// normalize a TIMESTAMP settlement_date down to an epoch-day.
constexpr atx::i64 kNsPerDay = 86'400LL * 1'000'000'000LL;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Floor-divide ns by ns/day toward negative infinity so a pre-epoch timestamp
// (negative ns) still maps to the correct (negative) epoch-day. C++ integer
// division truncates toward zero, which is wrong for negatives; correct it.
[[nodiscard]] atx::i64 nanos_to_epoch_day(atx::i64 ns) noexcept {
  atx::i64 q = ns / kNsPerDay;
  if (ns % kNsPerDay != 0 && ns < 0) {
    --q;
  }
  return q;
}

// Read a numeric column as f64 regardless of its on-disk physical type (FINRA
// stores quantities as int64 and dtc/change_percent as float64). Missing column
// -> empty vector + `present=false`.
struct NumCol {
  std::vector<atx::f64> v;
  bool present{false};
};

[[nodiscard]] Result<NumCol> read_numeric_as_f64(const atx::core::io::ParquetTable& table,
                                                 std::string_view name) {
  NumCol out;
  const auto* info = table.schema().find(name);
  if (info == nullptr) {
    return Ok(std::move(out)); // absent -> present=false
  }
  out.present = true;
  using DT = atx::core::io::DType;
  switch (info->dtype) {
  case DT::Float64:
  case DT::Float32: {
    ATX_TRY(auto col, table.column_view<atx::f64>(name));
    out.v.assign(col.begin(), col.end());
    break;
  }
  case DT::Int64:
  case DT::Int32:
  case DT::Int16:
  case DT::Int8:
  case DT::UInt64:
  case DT::UInt32:
  case DT::UInt16:
  case DT::UInt8: {
    ATX_TRY(auto col, table.column_view<atx::i64>(name));
    out.v.reserve(col.size());
    for (atx::i64 x : col) {
      out.v.push_back(static_cast<atx::f64>(x));
    }
    break;
  }
  default:
    return Err(ErrorCode::InvalidArgument,
               std::string{"finra: column '"} + std::string{name} + "' has an unsupported dtype");
  }
  return Ok(std::move(out));
}

// Read settlement_date as epoch-days, accepting either DATE32 (real downloader)
// or a midnight-UTC TIMESTAMP (what write_parquet can synthesize). Err if the
// column is absent or an unexpected type.
[[nodiscard]] Result<std::vector<atx::i64>>
read_settlement_epoch_days(const atx::core::io::ParquetTable& table) {
  using DT = atx::core::io::DType;
  const auto* info = table.schema().find("settlement_date");
  if (info == nullptr) {
    return Err(ErrorCode::InvalidArgument, "finra: 'settlement_date' column absent");
  }
  std::vector<atx::i64> out;
  if (info->dtype == DT::Date32) {
    ATX_TRY(auto days, table.date32_days("settlement_date"));
    out.reserve(days.size());
    for (atx::i32 d : days) {
      out.push_back(static_cast<atx::i64>(d));
    }
    return Ok(std::move(out));
  }
  if (info->dtype == DT::Timestamp) {
    ATX_TRY(auto ts, table.to_column<atx::core::time::Timestamp>("settlement_date"));
    const auto view = ts.view();
    out.reserve(view.size());
    for (const auto& t : view) {
      out.push_back(nanos_to_epoch_day(t.unix_nanos()));
    }
    return Ok(std::move(out));
  }
  return Err(ErrorCode::InvalidArgument,
             "finra: 'settlement_date' must be DATE32 or TIMESTAMP");
}

// Unscheduled full-day NYSE closures inside the supported calendar range. The rule
// calendar in atx-core does not model these (datetime.hpp "NOT modelled").
constexpr std::array<std::array<unsigned, 3>, 11> kNyseUnscheduledClosures{{
    {1994U, 4U, 27U},  // President Nixon's funeral
    {2001U, 9U, 11U},  // September 11 attacks
    {2001U, 9U, 12U},
    {2001U, 9U, 13U},
    {2001U, 9U, 14U},
    {2004U, 6U, 11U},  // President Reagan's funeral
    {2007U, 1U, 2U},   // President Ford's day of mourning
    {2012U, 10U, 29U}, // Hurricane Sandy
    {2012U, 10U, 30U},
    {2018U, 12U, 5U},  // President G.H.W. Bush's day of mourning
    {2025U, 1U, 9U},   // President Carter's day of mourning
}};

[[nodiscard]] bool is_unscheduled_closure(const atx::core::time::Date& d) noexcept {
  for (const auto& c : kNyseUnscheduledClosures) {
    if (d.year == static_cast<atx::i32>(c[0]) && d.month == c[1] && d.day == c[2]) {
      return true;
    }
  }
  return false;
}

// Session test without the range check (callers validate the range first).
[[nodiscard]] bool nyse_session_unchecked(atx::i64 day) noexcept {
  const atx::core::time::Date d = atx::core::time::Date::from_days(day);
  if (atx::core::time::is_weekend(d) || is_unscheduled_closure(d)) {
    return false;
  }
  // The NYSE first observed Martin Luther King Jr. Day in 1998; the atx-core rule
  // calendar applies it to every year, so re-open that Monday before 1998.
  if (d.year < 1998 &&
      d == atx::core::time::nth_weekday_of_month(d.year, 1, atx::core::time::Weekday::Monday, 3)) {
    return true;
  }
  const atx::core::time::Calendar nyse{};
  return !nyse.is_holiday(d);
}

[[nodiscard]] bool in_nyse_range(atx::i64 day) noexcept {
  return day >= kNyseCalendarFirstDay && day <= kNyseCalendarLastDay;
}

// One causal observation for an instrument column: visible from publish_day on.
struct Obs {
  atx::i64 publish_day{};
  atx::f64 dtc{kNaN};
  atx::f64 short_qty{kNaN};
  atx::f64 adv{kNaN};
  atx::f64 chg{kNaN};
};

} // namespace

atx::core::Result<bool> nyse_is_session(atx::i64 day) {
  if (!in_nyse_range(day)) {
    return Err(ErrorCode::OutOfRange, "nyse_is_session: day outside the 1990-2040 calendar");
  }
  return Ok(nyse_session_unchecked(day));
}

atx::core::Result<atx::i64> nyse_add_sessions(atx::i64 day, int n) {
  if (n < 0) {
    return Err(ErrorCode::InvalidArgument, "nyse_add_sessions: n must be >= 0");
  }
  if (!in_nyse_range(day)) {
    return Err(ErrorCode::OutOfRange, "nyse_add_sessions: day outside the 1990-2040 calendar");
  }
  atx::i64 cur = day;
  int found = 0;
  // Bounded: every step advances `cur` by one day and the range check stops the
  // walk at the calendar's last day (at most ~19k iterations).
  while (found < n) {
    ++cur;
    if (!in_nyse_range(cur)) {
      return Err(ErrorCode::OutOfRange,
                 "nyse_add_sessions: result falls after the 1990-2040 calendar");
    }
    if (nyse_session_unchecked(cur)) {
      ++found;
    }
  }
  return Ok(cur);
}

atx::core::Result<atx::i64> finra_first_usable_day(atx::i64 settlement_day,
                                                   const FinraPublicationLag& lag) {
  if (lag.lag < 0) {
    return Err(ErrorCode::InvalidArgument, "finra_first_usable_day: lag must be >= 0");
  }
  switch (lag.rule) {
  case FinraLagRule::CalendarDaysV1:
    return Ok(settlement_day + static_cast<atx::i64>(lag.lag));
  case FinraLagRule::NyseSessionsV2: {
    // lag <= INT_MAX - 1 cannot overflow here: an int lag this large already fails
    // the calendar range inside nyse_add_sessions long before the addition matters.
    const int sessions = (lag.after_close && lag.lag < std::numeric_limits<int>::max())
                             ? lag.lag + 1
                             : lag.lag;
    return nyse_add_sessions(settlement_day, sessions);
  }
  }
  return Err(ErrorCode::InvalidArgument, "finra_first_usable_day: unknown FinraLagRule");
}

atx::core::Result<FinraFeatures> load_finra_features(
    const std::string& short_interest_root, std::span<const DateKey> panel_dates,
    const std::unordered_map<std::string, InstKey>& sym_to_inst, atx::usize instruments,
    std::span<const atx::f64> shares, int publication_lag_days) {
  return load_finra_features(short_interest_root, panel_dates, sym_to_inst, instruments, shares,
                             FinraPublicationLag{FinraLagRule::CalendarDaysV1,
                                                 publication_lag_days, /*after_close=*/false});
}

atx::core::Result<FinraFeatures> load_finra_features(
    const std::string& short_interest_root, std::span<const DateKey> panel_dates,
    const std::unordered_map<std::string, InstKey>& sym_to_inst, atx::usize instruments,
    std::span<const atx::f64> shares, const FinraPublicationLag& lag) {
  const atx::usize D = panel_dates.size();
  const atx::usize N = instruments;
  if (lag.lag < 0) {
    return Err(ErrorCode::InvalidArgument, "load_finra_features: publication lag must be >= 0");
  }

  // ---- Validate the axis ------------------------------------------------
  for (atx::usize d = 1; d < D; ++d) {
    if (panel_dates[d] <= panel_dates[d - 1]) {
      return Err(ErrorCode::InvalidArgument,
                 "load_finra_features: panel_dates must be strictly ascending");
    }
  }
  for (const auto& [sym, inst] : sym_to_inst) {
    if (static_cast<atx::usize>(inst) >= N) {
      return Err(ErrorCode::InvalidArgument,
                 "load_finra_features: sym_to_inst value out of instrument range");
    }
  }
  const atx::usize cells = D * N;
  if (!shares.empty() && shares.size() != cells) {
    return Err(ErrorCode::InvalidArgument,
               "load_finra_features: shares span must be dates*instruments or empty");
  }

  // ---- Enumerate date=YYYY-MM-DD partitions -----------------------------
  std::error_code ec;
  if (!fs::exists(fs::path{short_interest_root}, ec)) {
    return Err(ErrorCode::IoError,
               "load_finra_features: short-interest root does not exist: " + short_interest_root);
  }
  std::vector<std::string> parquet_paths;
  {
    fs::recursive_directory_iterator it{fs::path{short_interest_root}, ec};
    if (ec) {
      return Err(ErrorCode::IoError,
                 "load_finra_features: cannot iterate short-interest root: " + short_interest_root);
    }
    for (const auto& entry : it) {
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      const fs::path& p = entry.path();
      if (p.extension() == ".parquet") {
        const std::string fn = p.filename().string();
        if (!fn.empty() && fn.front() == '_') {
          continue; // skip _metadata/_common_metadata sidecars
        }
        parquet_paths.push_back(p.string());
      }
    }
  }
  if (parquet_paths.empty()) {
    return Err(ErrorCode::IoError,
               "load_finra_features: no .parquet partitions under " + short_interest_root);
  }
  // Deterministic processing order (path-sorted).
  std::sort(parquet_paths.begin(), parquet_paths.end());

  // ---- Accumulate causal observations per instrument column -------------
  std::vector<std::vector<Obs>> per_inst(N);
  std::unordered_map<std::string, bool> unmatched_syms; // distinct dropped FINRA symbols
  atx::usize rows_read = 0;
  atx::usize rows_placed = 0;

  // settlement_day -> first usable day, memoized (a partition shares one or two
  // settlement dates across thousands of rows; the session walk runs once each).
  std::unordered_map<atx::i64, atx::i64> usable_day_of;

  for (const std::string& path : parquet_paths) {
    ATX_TRY(auto table, atx::core::io::read_parquet(path));
    const atx::i64 nrows = table.num_rows();
    if (nrows == 0) {
      continue;
    }

    ATX_TRY(auto settle_days, read_settlement_epoch_days(table));
    ATX_TRY(auto syms, table.strings("symbol"));
    ATX_TRY(auto dtc_col, read_numeric_as_f64(table, "days_to_cover_quantity"));
    ATX_TRY(auto short_col, read_numeric_as_f64(table, "current_short_position_quantity"));
    ATX_TRY(auto adv_col, read_numeric_as_f64(table, "average_daily_volume_quantity"));
    ATX_TRY(auto chg_col, read_numeric_as_f64(table, "change_percent"));

    const atx::usize rows = static_cast<atx::usize>(nrows);
    rows_read += rows;

    for (atx::usize r = 0; r < rows; ++r) {
      const std::string sym{syms[r]};
      const auto it = sym_to_inst.find(sym);
      if (it == sym_to_inst.end()) {
        unmatched_syms[sym] = true;
        continue; // symbol not in this panel's universe -> dropped (never misplaced)
      }
      const atx::usize inst = static_cast<atx::usize>(it->second);

      Obs o;
      const atx::i64 settle = settle_days[r];
      auto usable = usable_day_of.find(settle);
      if (usable == usable_day_of.end()) {
        ATX_TRY(const atx::i64 first_usable, finra_first_usable_day(settle, lag));
        usable = usable_day_of.emplace(settle, first_usable).first;
      }
      o.publish_day = usable->second;
      o.dtc = dtc_col.present ? dtc_col.v[r] : kNaN;
      o.short_qty = short_col.present ? short_col.v[r] : kNaN;
      o.adv = adv_col.present ? adv_col.v[r] : kNaN;
      o.chg = chg_col.present ? chg_col.v[r] : kNaN;
      per_inst[inst].push_back(o);
      ++rows_placed;
    }
  }

  // Sort each instrument's observations by publish_day (ascending). On a tie keep
  // a stable order; the as-of placement below takes the last <= the panel date.
  for (auto& obs : per_inst) {
    std::stable_sort(obs.begin(), obs.end(),
                     [](const Obs& a, const Obs& b) { return a.publish_day < b.publish_day; });
  }

  // ---- Project onto the (D x N) panel axis with as-of forward-fill ------
  FinraFeatures out;
  out.dates = D;
  out.instruments = N;
  out.si_dtc.assign(cells, kNaN);
  out.si_util.assign(cells, kNaN);
  out.si_chg.assign(cells, kNaN);
  out.rows_read = rows_read;
  out.rows_placed = rows_placed;
  out.symbols_unmatched = unmatched_syms.size();

  for (atx::usize inst = 0; inst < N; ++inst) {
    const std::vector<Obs>& obs = per_inst[inst];
    if (obs.empty()) {
      continue; // uncovered instrument: all NaN (never imputed)
    }
    // Walk panel dates ascending; advance an as-of cursor to the newest obs whose
    // publish_day <= the current panel date. Because panel_dates ascend and obs
    // are publish-sorted, this is a single linear merge (no per-date search).
    atx::usize cursor = 0; // number of obs already published as of the prior date
    std::optional<atx::usize> active;
    for (atx::usize d = 0; d < D; ++d) {
      const atx::i64 today = static_cast<atx::i64>(panel_dates[d]);
      while (cursor < obs.size() && obs[cursor].publish_day <= today) {
        active = cursor;
        ++cursor;
      }
      if (!active.has_value()) {
        continue; // no obs visible yet on this date -> NaN (no look-ahead)
      }
      const Obs& a = obs[*active];
      const atx::usize cell = d * N + inst;
      out.si_dtc[cell] = a.dtc;
      out.si_chg[cell] = a.chg;

      // si_util = short / shares (panel float) with ADV fallback.
      atx::f64 denom = kNaN;
      bool from_shares = false;
      if (!shares.empty()) {
        const atx::f64 sh = shares[cell];
        if (!std::isnan(sh) && sh > 0.0) {
          denom = sh;
          from_shares = true;
        }
      }
      if (!from_shares) {
        if (!std::isnan(a.adv) && a.adv > 0.0) {
          denom = a.adv;
        }
      }
      if (!std::isnan(a.short_qty) && !std::isnan(denom) && denom > 0.0) {
        out.si_util[cell] = a.short_qty / denom;
        if (from_shares) {
          ++out.util_from_shares;
        } else {
          ++out.util_from_adv;
        }
      }
    }
  }

  return Ok(std::move(out));
}

} // namespace atx::engine::data
