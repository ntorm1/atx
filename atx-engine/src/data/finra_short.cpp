// atx::engine::data — FINRA consolidated short-interest loader (Track B1).
//
// Reads the hive-partitioned FINRA parquet (date=YYYY-MM-DD/part-*.parquet) and
// projects three derived, CAUSALLY placed feature columns onto an externally
// supplied research-panel axis. See finra_short.hpp for the contract; the
// causality model is: a (symbol, settlement_day) observation becomes visible on
// panel dates >= settlement_day + publication_lag_days (calendar days) and is
// forward-filled until the next observation for that symbol becomes visible.
//
// Parquet is read only through atx::core::io::read_parquet (PIMPL; no Arrow
// headers here), mirroring atx-tsdb/src/load_parquet.cpp.

#include "atx/engine/data/finra_short.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <filesystem>

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

// One causal observation for an instrument column: visible from publish_day on.
struct Obs {
  atx::i64 publish_day{};
  atx::f64 dtc{kNaN};
  atx::f64 short_qty{kNaN};
  atx::f64 adv{kNaN};
  atx::f64 chg{kNaN};
};

} // namespace

atx::core::Result<FinraFeatures> load_finra_features(
    const std::string& short_interest_root, std::span<const DateKey> panel_dates,
    const std::unordered_map<std::string, InstKey>& sym_to_inst, atx::usize instruments,
    std::span<const atx::f64> shares, int publication_lag_days) {
  const atx::usize D = panel_dates.size();
  const atx::usize N = instruments;

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

  const atx::i64 lag = static_cast<atx::i64>(publication_lag_days);

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
      o.publish_day = settle_days[r] + lag;
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
