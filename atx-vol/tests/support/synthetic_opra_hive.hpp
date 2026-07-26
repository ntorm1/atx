#pragma once

// atx-vol test support: synthetic multi-symbol OPRA hive fixture.
//
// Generates deterministic option-chain rows and writes them in two on-disk
// layouts used as the test bed for the surface-db production loader (Task 3) and
// build driver (Task 5):
//
//   v2 (NEW): <root>/date=<YYYY-MM-DD>/data.parquet   — one file per date holding
//             ALL symbols, the 8-column OPRA schema (ts, underlying, symbol,
//             instrument_id, bid_px, ask_px, bid_sz, ask_sz), rows sorted by
//             (underlying, symbol). Produced via write_hive_parquet on a `date`
//             partition column (which drops `date` from each file).
//   v1 (OLD): <root>/<symbol>/<date>.parquet          — one file per (symbol,date)
//             for parity/migration tests.
//
// Per (symbol, date): 9 strikes K in {80,85,...,120}, 2 expiries at trade date
// +28d and +56d, both a call and a put per strike (put-call-parity pairs so the
// loader can imply the spot). The mid is a flat-rate Black European price under a
// quadratic vol smile sigma(K) = 0.25 + 0.02*((K/S)-1)^2; bid = 0.98*mid,
// ask = 1.02*mid, both stored as int64 fixed-point round(px * 1e9); sizes = 10.
//
// Self-contained: NO atx pricing/vol headers are pulled into test support. The
// Black mid uses a local erfc-based standard-normal CDF (see detail::black_price).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "atx/core/datetime.hpp"          // time::Date, timestamp_from_utc
#include "atx/core/io/parquet_writer.hpp" // WriteColumn, write_parquet, write_hive_parquet
#include "atx/core/types.hpp"             // atx::i64

namespace atx::vol::testsupport {

// Tunables for the generated hive. Defaults produce a 3-symbol x 3-date board.
struct SyntheticHiveSpec {
  std::vector<std::string> symbols{{"AAA", "BBB", "CCC"}};
  std::vector<std::string> dates{{"2026-07-01", "2026-07-02", "2026-07-06"}};
  double spot{100.0};
  double r{0.03};
  // Truncate each (symbol, date) cell to this many quote rows. 0 (default) = the
  // full 36-row board (9 strikes x 2 expiries x {C,P}). A small value writes a
  // hive whose boards are REAL but unselectable, which is how a case reproduces
  // the fail-closed "config selection failed" path FROM DISK rather than gutting
  // `CorpusBoard::frame.rows` in memory after the load. The regimes are narrow:
  // 1 row is rejected by the loader itself (the cell never reaches config), 2
  // rows load cleanly and fail curve selection, and >= 4 rows select fine.
  std::size_t max_rows_per_cell{0};
  // Symbols whose OSI root gets a trailing "1" appended (the OCC adjusted /
  // non-standard-deliverable convention). Such a row's `symbol` column then
  // disagrees with its `underlying` column by MORE than punctuation — "AAA1" vs
  // "AAA" — which is the real divergence class `pull_opra_hive.py`'s trailing-digit
  // strip can manufacture, and the one the loader must refuse rather than merge
  // into the vanilla chain. Empty (default) = every symbol is its own root.
  std::vector<std::string> adjusted_root_symbols{};
};

namespace detail {

// Standard-normal CDF via erfc (keeps this fixture free of atx pricing headers).
[[nodiscard]] inline double norm_cdf(double x) {
  return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

// Flat-rate Black (q = 0) European option price. Self-contained pricer used only
// to plant put-call-parity-consistent mids on the fixture.
[[nodiscard]] inline double black_price(double s, double k, double t, double r, double sigma,
                                        bool is_call) {
  const double sig_sqrt_t = sigma * std::sqrt(t);
  const double d1 = (std::log(s / k) + (r + 0.5 * sigma * sigma) * t) / sig_sqrt_t;
  const double d2 = d1 - sig_sqrt_t;
  const double disc = std::exp(-r * t);
  if (is_call) {
    return s * norm_cdf(d1) - k * disc * norm_cdf(d2);
  }
  return k * disc * norm_cdf(-d2) - s * norm_cdf(-d1);
}

// One generated NBBO row (fixed-point prices, running-counter id assigned later).
struct SynthRow {
  std::string date;       // "YYYY-MM-DD" trade date (the v2 hive partition value)
  std::string underlying; // plain symbol, e.g. "AAA"
  std::string symbol;     // OSI/OCC 21-char, e.g. "AAA   260729C00100000"
  atx::i64 ts_ns{0};      // "<date>T19:55:00Z" as unix nanoseconds
  atx::i64 bid_px{0};     // round(0.98 * mid * 1e9)
  atx::i64 ask_px{0};     // round(1.02 * mid * 1e9)
};

// Parse a "YYYY-MM-DD" string into a civil Date.
[[nodiscard]] inline atx::core::time::Date parse_iso_date(const std::string &d) {
  return atx::core::time::Date{
      static_cast<atx::i32>(std::stoi(d.substr(0, 4))),
      static_cast<atx::u32>(std::stoi(d.substr(5, 2))),
      static_cast<atx::u32>(std::stoi(d.substr(8, 2)))};
}

// Format a Date as the 6-char OSI expiry field "YYMMDD".
[[nodiscard]] inline std::string yymmdd(atx::core::time::Date d) {
  char buf[7];
  std::snprintf(buf, sizeof(buf), "%02d%02d%02d", static_cast<int>(d.year % 100),
                static_cast<int>(d.month), static_cast<int>(d.day));
  return std::string(buf);
}

// The OSI root a real feed carries for a universe symbol: the ticker with its
// punctuation removed (`pull_opra_hive.py`'s `sym.replace(".", "")`, and OCC's
// own class-share convention — `BRK.B` trades as `BRKB`). A dot-free ticker is
// its own root, so every pre-existing fixture symbol is byte-for-byte unchanged;
// only a punctuated one gains the real-world property that its `underlying`
// column and its `symbol` column disagree. That divergence is the whole point of
// the fixture: it is what a synthetic hive needs in order to reproduce the
// production BRK.B loss.
[[nodiscard]] inline std::string osi_root(std::string symbol) {
  symbol.erase(std::remove(symbol.begin(), symbol.end(), '.'), symbol.end());
  return symbol;
}

// Compose an OSI/OCC 21-char symbol: 6-char space-padded root + YYMMDD + {C|P} +
// 8-digit strike (price * 1000, zero-padded).
[[nodiscard]] inline std::string osi_symbol(std::string root, const std::string &ym, char cp,
                                            double strike) {
  root = osi_root(std::move(root));
  root.resize(6, ' ');
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08lld", static_cast<long long>(std::llround(strike * 1000.0)));
  return root + ym + std::string(1, cp) + std::string(buf);
}

// Generate the 36 rows (9 strikes x 2 expiries x {C,P}) for one (symbol, date),
// appending them to `out`.
inline void append_symbol_date_rows(const std::string &symbol, const std::string &date,
                                    const SyntheticHiveSpec &spec, std::vector<SynthRow> &out) {
  using atx::core::time::Date;
  using atx::core::time::timestamp_from_utc;

  const Date trade = parse_iso_date(date);
  const atx::i64 ts_ns =
      timestamp_from_utc(trade.year, trade.month, trade.day, 19U, 55U, 0U, 0U).unix_nanos();

  // The root this symbol's contracts trade under. Normally the dot-stripped
  // ticker; with `adjusted_root_symbols` it also carries the OCC trailing-digit
  // adjustment marker, so `symbol` and `underlying` name different underliers.
  const bool adjusted = std::find(spec.adjusted_root_symbols.begin(),
                                  spec.adjusted_root_symbols.end(),
                                  symbol) != spec.adjusted_root_symbols.end();
  const std::string root = adjusted ? osi_root(symbol) + "1" : symbol;

  const int dtes[] = {28, 56};
  const double strikes[] = {80.0, 85.0, 90.0, 95.0, 100.0, 105.0, 110.0, 115.0, 120.0};
  constexpr double kNsPerYear = 365.0 * 24.0 * 3600.0 * 1e9;
  const auto to_px = [](double px) { return static_cast<atx::i64>(std::llround(px * 1e9)); };

  for (const int dte : dtes) {
    const Date exp = Date::from_days(trade.to_days() + dte);
    const std::string ym = yymmdd(exp);
    // Midnight-UTC expiry instant for the pricing T (the loader default); the
    // exact convention is immaterial here — put-call parity is r-driven and the
    // implied-spot tolerance is 1%.
    const atx::i64 exp_ns = timestamp_from_utc(exp.year, exp.month, exp.day, 0U, 0U, 0U, 0U)
                                .unix_nanos();
    const double t = static_cast<double>(exp_ns - ts_ns) / kNsPerYear;
    for (const double k : strikes) {
      const double m = (k / spec.spot) - 1.0;
      const double sigma = 0.25 + 0.02 * m * m;
      for (const char cp : {'C', 'P'}) {
        const double mid = black_price(spec.spot, k, t, spec.r, sigma, cp == 'C');
        out.push_back(SynthRow{date, symbol, osi_symbol(root, ym, cp, k), ts_ns,
                               to_px(0.98 * mid), to_px(1.02 * mid)});
      }
    }
  }
}

} // namespace detail

// Writes the NEW hive-v2 layout: <root>/date=<d>/data.parquet, one file per date
// holding all symbols' rows sorted by (underlying, symbol), with the full
// 8-column OPRA schema. `date` is the hive partition column (dropped per file).
inline void write_synthetic_hive_v2(const std::filesystem::path &root,
                                    const SyntheticHiveSpec &spec) {
  namespace io = atx::core::io;

  std::vector<detail::SynthRow> rows;
  for (const std::string &date : spec.dates) {
    for (const std::string &sym : spec.symbols) {
      const std::size_t before = rows.size();
      detail::append_symbol_date_rows(sym, date, spec, rows);
      if (spec.max_rows_per_cell > 0 && rows.size() - before > spec.max_rows_per_cell) {
        rows.resize(before + spec.max_rows_per_cell);
      }
    }
  }
  // Sort by (date, underlying, symbol) so each date bucket write_hive_parquet
  // emits is internally ordered (underlying, symbol).
  std::stable_sort(rows.begin(), rows.end(), [](const detail::SynthRow &a, const detail::SynthRow &b) {
    if (a.date != b.date) {
      return a.date < b.date;
    }
    if (a.underlying != b.underlying) {
      return a.underlying < b.underlying;
    }
    return a.symbol < b.symbol;
  });

  // Backing storage for the borrowed WriteColumn spans (must outlive the write).
  std::vector<atx::i64> ts, inst, bid_px, ask_px, bid_sz, ask_sz;
  std::vector<std::string> und, symc, datec;
  atx::i64 counter = 1;
  for (const detail::SynthRow &rw : rows) {
    datec.push_back(rw.date);
    und.push_back(rw.underlying);
    symc.push_back(rw.symbol);
    inst.push_back(counter++);
    ts.push_back(rw.ts_ns);
    bid_px.push_back(rw.bid_px);
    ask_px.push_back(rw.ask_px);
    bid_sz.push_back(10);
    ask_sz.push_back(10);
  }

  const std::vector<io::WriteColumn> cols = {
      {"ts", std::span<const atx::i64>(ts)},
      {"underlying", std::span<const std::string>(und)},
      {"symbol", std::span<const std::string>(symc)},
      {"instrument_id", std::span<const atx::i64>(inst)},
      {"bid_px", std::span<const atx::i64>(bid_px)},
      {"ask_px", std::span<const atx::i64>(ask_px)},
      {"bid_sz", std::span<const atx::i64>(bid_sz)},
      {"ask_sz", std::span<const atx::i64>(ask_sz)},
      {"date", std::span<const std::string>(datec)},
  };
  (void)io::write_hive_parquet(cols, root.string(), "date");
}

// Writes the OLD per-symbol layout: <root>/<symbol>/<date>.parquet (one file per
// (symbol, date)), each carrying the same 8-column OPRA schema. Used for
// parity/migration tests against the v2 layout.
inline void write_synthetic_hive_v1(const std::filesystem::path &root,
                                    const SyntheticHiveSpec &spec) {
  namespace io = atx::core::io;

  atx::i64 counter = 1;
  for (const std::string &sym : spec.symbols) {
    for (const std::string &date : spec.dates) {
      std::vector<detail::SynthRow> rows;
      detail::append_symbol_date_rows(sym, date, spec, rows);

      std::vector<atx::i64> ts, inst, bid_px, ask_px, bid_sz, ask_sz;
      std::vector<std::string> und, symc;
      for (const detail::SynthRow &rw : rows) {
        und.push_back(rw.underlying);
        symc.push_back(rw.symbol);
        inst.push_back(counter++);
        ts.push_back(rw.ts_ns);
        bid_px.push_back(rw.bid_px);
        ask_px.push_back(rw.ask_px);
        bid_sz.push_back(10);
        ask_sz.push_back(10);
      }

      const std::vector<io::WriteColumn> cols = {
          {"ts", std::span<const atx::i64>(ts)},
          {"underlying", std::span<const std::string>(und)},
          {"symbol", std::span<const std::string>(symc)},
          {"instrument_id", std::span<const atx::i64>(inst)},
          {"bid_px", std::span<const atx::i64>(bid_px)},
          {"ask_px", std::span<const atx::i64>(ask_px)},
          {"bid_sz", std::span<const atx::i64>(bid_sz)},
          {"ask_sz", std::span<const atx::i64>(ask_sz)},
      };
      const std::filesystem::path path = root / sym / (date + ".parquet");
      (void)io::write_parquet(cols, path.string());
    }
  }
}

} // namespace atx::vol::testsupport
