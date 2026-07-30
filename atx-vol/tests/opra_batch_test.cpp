#include "atx/vol/opra_batch.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/io/parquet_writer.hpp"
#include "atx/vol/american.hpp"     // american_price
#include "atx/vol/chain.hpp"        // OptionChain
#include "atx/vol/curve.hpp"        // YieldCurve
#include "atx/vol/data.hpp"         // QuoteFrame
#include "atx/vol/market_env.hpp"   // MarketEnv
#include "atx/vol/panel.hpp"        // make_synthetic_american_panel
#include "atx/vol/pricer_fitter.hpp"   // PricerFitter
#include "atx/vol/opra_hive.hpp"       // OpraHiveSpec, load_opra_hive (shared date gate)
#include "atx/vol/spy_fixture.hpp"     // make_spy_synthetic_spec
#include "atx/vol/surface_archive.hpp" // SurfaceArchive

// R1-c (review C-07): `parse_civil` is the src-private civil-date kernel BOTH
// loaders gate their date range on, and it had no test of its own. Everything in
// the header is `inline`, so including it costs no TU and no link surface.
#include "../src/opra_batch_detail.hpp"

// Coverage for the P2-4 date-range batch loader (`load_opra_daterange`) and the
// term-curve -> MarketEnv bridge (`market_env_from_frame`).

namespace {

namespace io = atx::core::io;
namespace fs = std::filesystem;
namespace opra_detail = atx::vol::opra_detail; // R1-c: the civil-date kernel
using atx::i64;
using atx::vol::CorpusMarketInputCell;
using atx::vol::CorpusMarketInputTable;
using atx::vol::load_opra_date_windows;
using atx::vol::load_opra_daterange;
using atx::vol::market_env_from_frame;
using atx::vol::MarketEnv;
using atx::vol::MissingMarketInputPolicy;
using atx::vol::OpraBatchEntry;
using atx::vol::OpraBatchResult;
using atx::vol::OpraBatchSpec;
using atx::vol::OpraWindowLoadStats;
using atx::vol::QuoteFrame;
using atx::vol::YieldCurve;

// ── In-test parquet fixture ────────────────────────────────────────────────

// Compose an OSI/OCC 21-char symbol: 6-char space-padded root + YYMMDD + C/P +
// 8-digit strike (price x 1000).
[[nodiscard]] std::string osi_sym(std::string root, const std::string& yymmdd, char cp,
                                  double strike) {
  root.resize(6, ' ');
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08lld",
                static_cast<long long>(std::llround(strike * 1000.0)));
  return root + yymmdd + std::string(1, cp) + std::string(buf);
}

// Write a single-symbol co-terminal call/put pair (one strike, one expiry) to an
// explicit parquet path so the loader can imply the spot via put-call parity.
// The mids are planted so C - P = fwd - strike (r = 0): the implied forward, and
// thus the implied spot (df = 1), is `fwd`.
//
// The `ts` column carries `<date>T19:55:00Z`, derived from the file's own name
// (the v1 layout is `<symbol>/<date>.parquet`) at both loaders' default
// `snapshot_suffix`. Since FIX-C-1 the panel loader takes the snapshot instant
// FROM the file and rejects a `snapshot_iso` that disagrees with it, so a fixture
// whose `ts` is an unrelated placeholder while the loader stamps
// `date + snapshot_suffix` is not a valid date file at all -- it asserts two
// different snapshot instants at once. Deriving it keeps every case
// self-consistent and leaves every T unchanged: the stamp the math uses is the
// same string as before, only the previously-inert `ts` column moved to agree.
void write_pair(const fs::path &path, const std::string &symbol, const std::string &yymmdd,
                double strike, double fwd, bool with_instrument_ids = false) {
  const double put_mid = 5.0;
  const double call_mid = put_mid + (fwd - strike);
  const auto to_px = [](double d) { return static_cast<i64>(std::llround(d * 1e9)); };
  const i64 snap_ns = atx::vol::iso_to_ns(path.stem().string() + "T19:55:00Z");

  std::vector<i64> ts_col = {snap_ns, snap_ns};
  std::vector<std::string> und_col = {symbol, symbol};
  std::vector<std::string> sym_col = {osi_sym(symbol, yymmdd, 'C', strike),
                                      osi_sym(symbol, yymmdd, 'P', strike)};
  std::vector<i64> bidpx = {to_px(call_mid - 0.05), to_px(put_mid - 0.05)};
  std::vector<i64> askpx = {to_px(call_mid + 0.05), to_px(put_mid + 0.05)};
  std::vector<i64> bidsz = {10, 10};
  std::vector<i64> asksz = {12, 12};
  std::vector<i64> instrument_ids = {1001, 1002};

  std::vector<io::WriteColumn> cols = {
      {"ts", std::span<const i64>(ts_col)},
      {"underlying", std::span<const std::string>(und_col)},
      {"symbol", std::span<const std::string>(sym_col)},
  };
  if (with_instrument_ids) {
    cols.push_back({"instrument_id", std::span<const i64>(instrument_ids)});
  }
  const std::vector<io::WriteColumn> quote_cols = {
      {"bid_px", std::span<const i64>(bidpx)},
      {"ask_px", std::span<const i64>(askpx)},
      {"bid_sz", std::span<const i64>(bidsz)},
      {"ask_sz", std::span<const i64>(asksz)},
  };
  cols.insert(cols.end(), quote_cols.begin(), quote_cols.end());
  fs::create_directories(path.parent_path());
  fs::remove(path);
  ASSERT_TRUE(io::write_parquet(cols, path.string()).has_value());
}

// The two symbols' planted forwards (implied spot with r = 0).
constexpr double kXomFwd = 111.0;
constexpr double kAaplFwd = 252.0;

// Find the entry for a (symbol, date) in the batch result.
[[nodiscard]] const OpraBatchEntry* find_entry(const OpraBatchResult& r,
                                               const std::string& symbol,
                                               const std::string& date) {
  for (const OpraBatchEntry& e : r.entries) {
    if (e.symbol == symbol && e.date == date) {
      return &e;
    }
  }
  return nullptr;
}

[[nodiscard]] atx::vol::ExternalInputTag tag(std::string source, std::string as_of) {
  return atx::vol::ExternalInputTag{std::move(source), std::move(as_of)};
}

[[nodiscard]] CorpusMarketInputCell market_cell(std::string date, std::string symbol,
                                                std::string as_of) {
  CorpusMarketInputCell cell;
  cell.date = std::move(date);
  cell.symbol = std::move(symbol);
  cell.provenance.spot = tag("opra-pcp", as_of);
  cell.provenance.rates = tag("curve-fixture", as_of);
  cell.provenance.dividends = tag("dividend-fixture", as_of);
  cell.provenance.fit_context = tag("calendar-fixture", std::move(as_of));
  return cell;
}

// ── load_opra_daterange: counts, panels, missing handling ──────────────────

TEST(OpraBatch, DateRange_CountsPanelsAndMissing) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_batch_counts";
  fs::remove_all(root);

  const std::vector<std::string> dates = {"2026-06-01", "2026-06-02", "2026-06-03"};
  // Expiry ~110 days out (well-conditioned for the PCP spot back-out).
  const std::string yymmdd = "260918";
  const std::string exp_iso = "2026-09-18";
  const double strike = 110.0;

  // 2 symbols x 3 dates = 6 cells, with (AAPL, 2026-06-02) DELIBERATELY absent.
  for (const std::string& d : dates) {
    write_pair(root / "XOM" / (d + ".parquet"), "XOM", yymmdd, strike, kXomFwd);
    if (d != "2026-06-02") {
      write_pair(root / "AAPL" / (d + ".parquet"), "AAPL", yymmdd, strike, kAaplFwd);
    }
  }

  OpraBatchSpec spec;
  spec.symbols = {"XOM", "AAPL"};
  spec.date_lo = "2026-06-01";
  spec.date_hi = "2026-06-03";
  spec.root_dir = root.string();
  spec.r = 0.0; // r = 0 => implied spot == planted forward exactly

  const auto res = load_opra_daterange(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  EXPECT_EQ(res->n_total, std::size_t{6});
  EXPECT_EQ(res->n_loaded, std::size_t{5});
  EXPECT_EQ(res->n_missing, std::size_t{1});
  EXPECT_EQ(res->n_error, std::size_t{0});
  EXPECT_EQ(res->entries.size(), std::size_t{6});

  // Every Ok entry's panel: single co-terminal pair (2 contracts) and the right
  // symbol-specific implied spot.
  for (const OpraBatchEntry& e : res->entries) {
    if (e.symbol == "AAPL" && e.date == "2026-06-02") {
      continue; // the missing one
    }
    ASSERT_TRUE(e.panel.has_value()) << e.symbol << " " << e.date << ": "
                                     << e.panel.error().to_string();
    EXPECT_EQ(e.panel->n_contracts, std::size_t{2});
    EXPECT_EQ(e.panel->n_expiries, std::size_t{1});
    EXPECT_EQ(e.panel->frame.uid, e.symbol);
    const double expected = (e.symbol == "XOM") ? kXomFwd : kAaplFwd;
    EXPECT_NEAR(e.panel->implied_spot, expected, 1e-3) << e.symbol << " " << e.date;
  }

  // The missing (AAPL, 2026-06-02) cell is Err(NotFound).
  const OpraBatchEntry* missing = find_entry(*res, "AAPL", "2026-06-02");
  ASSERT_NE(missing, nullptr);
  ASSERT_FALSE(missing->panel.has_value());
  EXPECT_EQ(missing->panel.error().code(), atx::vol::ErrorCode::NotFound);

  fs::remove_all(root);
}

TEST(OpraBatch, DateRange_ProgressFiresPerCellMonotonic) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_batch_progress";
  fs::remove_all(root);

  const std::vector<std::string> dates = {"2026-06-01", "2026-06-02", "2026-06-03"};
  for (const std::string& d : dates) {
    write_pair(root / "XOM" / (d + ".parquet"), "XOM", "260918", 110.0, kXomFwd);
    if (d != "2026-06-02") {
      write_pair(root / "AAPL" / (d + ".parquet"), "AAPL", "260918", 110.0, kAaplFwd);
    }
  }

  OpraBatchSpec spec;
  spec.symbols = {"XOM", "AAPL"};
  spec.date_lo = "2026-06-01";
  spec.date_hi = "2026-06-03";
  spec.root_dir = root.string();

  std::size_t n_calls = 0;
  std::size_t last_done = 0;
  std::size_t last_total = 0;
  bool monotonic = true;
  const auto progress = [&](std::size_t done, std::size_t total,
                            const OpraBatchEntry& /*entry*/) {
    ++n_calls;
    if (done != last_done + 1) {
      monotonic = false;
    }
    last_done = done;
    last_total = total;
  };

  const auto res = load_opra_daterange(spec, progress);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  EXPECT_EQ(n_calls, res->n_total);           // fired exactly n_total times
  EXPECT_EQ(n_calls, std::size_t{6});
  EXPECT_TRUE(monotonic);                      // done stepped 1,2,...,n_total
  EXPECT_EQ(last_done, res->n_total);
  EXPECT_EQ(last_total, res->n_total);

  fs::remove_all(root);
}

// ── Parallel vs serial load: n_threads must not change the result (W4.3) ─────
// Entries land by index into a pre-sized vector and counters are counted from the
// completed slots after the join, so the batch result is identical for any worker
// count. A wide grid with a hole makes a multi-worker queue finish cells out of
// index order -- any completion-order dependence would surface as a mismatch.
TEST(OpraBatch, DateRange_ParallelEqualsSerial) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_batch_parallel";
  fs::remove_all(root);

  const std::vector<std::string> dates = {"2026-06-01", "2026-06-02", "2026-06-03",
                                          "2026-06-04", "2026-06-05", "2026-06-08"};
  for (const std::string& d : dates) {
    write_pair(root / "XOM" / (d + ".parquet"), "XOM", "260918", 110.0, kXomFwd);
    if (d != "2026-06-03") {  // a deliberate AAPL hole -> a NotFound cell in the grid
      write_pair(root / "AAPL" / (d + ".parquet"), "AAPL", "260918", 110.0, kAaplFwd);
    }
  }

  OpraBatchSpec spec;
  spec.symbols = {"XOM", "AAPL"};
  spec.date_lo = "2026-06-01";
  spec.date_hi = "2026-06-08";
  spec.root_dir = root.string();
  spec.r = 0.0;

  spec.n_threads = 1;
  const auto serial = load_opra_daterange(spec);
  spec.n_threads = 8;
  const auto parallel = load_opra_daterange(spec);
  ASSERT_TRUE(serial.has_value()) << serial.error().to_string();
  ASSERT_TRUE(parallel.has_value()) << parallel.error().to_string();

  EXPECT_EQ(serial->n_total, parallel->n_total);
  EXPECT_EQ(serial->n_loaded, parallel->n_loaded);
  EXPECT_EQ(serial->n_missing, parallel->n_missing);
  EXPECT_EQ(serial->n_error, parallel->n_error);
  ASSERT_EQ(serial->entries.size(), parallel->entries.size());

  for (std::size_t i = 0; i < serial->entries.size(); ++i) {
    const OpraBatchEntry& s = serial->entries[i];
    const OpraBatchEntry& p = parallel->entries[i];
    EXPECT_EQ(s.symbol, p.symbol) << "entry " << i;
    EXPECT_EQ(s.date, p.date) << "entry " << i;
    EXPECT_EQ(s.panel.has_value(), p.panel.has_value()) << s.symbol << " " << s.date;
    if (s.panel.has_value()) {
      EXPECT_EQ(s.panel->n_contracts, p.panel->n_contracts) << s.symbol << " " << s.date;
      EXPECT_DOUBLE_EQ(s.panel->implied_spot, p.panel->implied_spot) << s.symbol << " " << s.date;
    } else {
      EXPECT_EQ(s.panel.error().code(), p.panel.error().code()) << s.symbol << " " << s.date;
    }
  }

  fs::remove_all(root);
}

// ── Malformed spec -> top-level Err ─────────────────────────────────────────

TEST(OpraBatch, ProductionDateWindowsBoundLoadedPanelWorkingSet) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_batch_windows";
  fs::remove_all(root);
  const std::vector<std::string> dates = {
      "2026-06-01", "2026-06-02", "2026-06-03", "2026-06-04", "2026-06-05"};
  for (const std::string &date : dates) {
    write_pair(root / "XOM" / (date + ".parquet"), "XOM", "260918", 110.0, kXomFwd);
    write_pair(root / "AAPL" / (date + ".parquet"), "AAPL", "260918", 110.0, kAaplFwd);
  }

  OpraBatchSpec spec;
  spec.symbols = {"XOM", "AAPL"};
  spec.date_lo = dates.front();
  spec.date_hi = dates.back();
  spec.root_dir = root.string();
  spec.n_threads = 2u;

  OpraWindowLoadStats stats;
  std::size_t windows_seen = 0u;
  std::size_t cells_seen = 0u;
  std::vector<std::string> dates_seen;
  const atx::vol::Status status = load_opra_date_windows(
      spec, 2u,
      [&](OpraBatchResult &&window) -> atx::vol::Status {
        ++windows_seen;
        cells_seen += window.entries.size();
        EXPECT_LE(window.entries.size(), 2u * spec.symbols.size())
            << "the production driver retained more than one configured date window";
        for (const OpraBatchEntry &entry : window.entries) {
          EXPECT_TRUE(entry.panel.has_value())
              << (entry.panel.has_value() ? std::string{} : entry.panel.error().to_string());
          if (dates_seen.empty() || dates_seen.back() != entry.date) {
            dates_seen.push_back(entry.date);
          }
        }
        return atx::core::Ok();
      },
      &stats);
  ASSERT_TRUE(status.has_value()) << status.error().to_string();
  EXPECT_EQ(windows_seen, 3u);
  EXPECT_EQ(stats.n_windows, windows_seen);
  EXPECT_EQ(stats.peak_entries, 4u);
  EXPECT_EQ(cells_seen, dates.size() * spec.symbols.size());
  EXPECT_EQ(dates_seen, dates);
  EXPECT_GT(stats.load_wall_s, 0.0);
  EXPECT_GE(stats.load_process_cpu_s, 0.0);

  fs::remove_all(root);
}

TEST(OpraBatch, MalformedSpec_EmptySymbols_Rejected) {
  OpraBatchSpec spec;
  spec.date_lo = "2026-06-01";
  spec.date_hi = "2026-06-03";
  spec.root_dir = fs::temp_directory_path().string();
  const auto res = load_opra_daterange(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(OpraBatch, MalformedSpec_ReversedDates_Rejected) {
  OpraBatchSpec spec;
  spec.symbols = {"XOM"};
  spec.date_lo = "2026-06-03";
  spec.date_hi = "2026-06-01"; // hi < lo
  spec.root_dir = fs::temp_directory_path().string();
  const auto res = load_opra_daterange(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(OpraBatch, MalformedSpec_UnparseableDate_Rejected) {
  OpraBatchSpec spec;
  spec.symbols = {"XOM"};
  spec.date_lo = "2026/06/01"; // wrong format
  spec.date_hi = "2026-06-03";
  spec.root_dir = fs::temp_directory_path().string();
  const auto res = load_opra_daterange(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

// ── R1-c (review C-07): impossible calendar dates must be REFUSED, not
// silently normalised ────────────────────────────────────────────────────────
//
// `parse_civil` validated only m in [1,12] and d in [1,31], so `2026-02-31`
// parsed. `days_from_civil` then normalised it -- its day-of-year arithmetic runs
// straight off the end of February -- and the range walk enumerated dates from
// `2026-03-03` onwards, i.e. a DIFFERENT window than the operator typed, with
// nothing anywhere saying so. It is now validated by round-tripping through
// `civil_from_days(days_from_civil(...))`.
//
// Tested directly against the src-private header because that is where the kernel
// lives (it is `inline`, so including it here costs nothing and adds no TU) --
// `parse_civil` had NO test of its own before this. The end-to-end consequence,
// which is what an operator sees, is pinned separately below through the real
// spec gate of both loaders.
TEST(OpraCivilDate, ImpossibleDatesAreRejected) {
  atx::vol::opra_detail::Civil c;

  // February past its end, in a non-leap year.
  EXPECT_FALSE(opra_detail::parse_civil("2026-02-29", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-02-30", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-02-31", c)); // the reviewer's case

  // A 30-day month's 31st.
  EXPECT_FALSE(opra_detail::parse_civil("2026-04-31", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-06-31", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-09-31", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-11-31", c));

  // Leap rules, all three branches: divisible by 4 (leap), by 100 (not), by 400
  // (leap). A days-in-month table gets these wrong far more easily than the
  // round trip does, which is the reason the round trip was chosen.
  EXPECT_TRUE(opra_detail::parse_civil("2024-02-29", c)); // 2024 is a leap year
  EXPECT_EQ(c.y, 2024);
  EXPECT_EQ(c.m, 2u);
  EXPECT_EQ(c.d, 29u);
  EXPECT_FALSE(opra_detail::parse_civil("1900-02-29", c)); // century, not a leap year
  EXPECT_TRUE(opra_detail::parse_civil("2000-02-29", c));  // divisible by 400, leap

  // The pre-existing rejections must still hold -- the range check has to run
  // BEFORE the round trip, because days_from_civil is only defined for
  // m in [1,12], d in [1,31].
  EXPECT_FALSE(opra_detail::parse_civil("2026-13-01", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-00-01", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-01-00", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-01-32", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026/01/01", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-1-1", c));
}

// ── REV-R3 (R1-c residual): a NEGATIVE year is not a date ────────────────────
//
// The round trip above cannot catch this one, and that is the whole point of
// testing it separately. `"-123-01-01"` is 10 bytes with dashes at [4] and [7],
// so it clears the shape gate; `std::from_chars` parses the year field into a
// SIGNED int as -123; month and day are in range; and `days_from_civil` /
// `civil_from_days` are defined for negative years, so it round-trips EXACTLY and
// R1-c's check passes it. The rejection therefore has to happen at the field
// parse: a civil-date field is unsigned by construction.
//
// Without this the loaders accepted a window starting in 124 BC and walked it as
// a contiguous serial-day interval — `date_lo = "-123-01-01"`, `date_hi =
// "2026-07-01"` is 784,000 days of directory probing, not an error.
TEST(OpraCivilDate, NegativeYearsAreRejected) {
  atx::vol::opra_detail::Civil c;

  EXPECT_FALSE(opra_detail::parse_civil("-123-01-01", c)) << "the reviewer's residual case";
  EXPECT_FALSE(opra_detail::parse_civil("-999-12-31", c));
  EXPECT_FALSE(opra_detail::parse_civil("-000-01-01", c)) << "negative zero is still a sign byte";

  // The same signed-parse hole in the month and day fields. These were already
  // refused, but only INCIDENTALLY, by the `m < 1` / `d < 1` range test — the
  // fields are now rejected where they are parsed, so the range test is no
  // longer the only thing standing behind them.
  EXPECT_FALSE(opra_detail::parse_civil("2026--1-01", c));
  EXPECT_FALSE(opra_detail::parse_civil("2026-01--1", c));

  // Other non-digit bytes in the year field, which had the same hole: anything
  // `from_chars` would stop on, or a space it would skip past, must be refused.
  EXPECT_FALSE(opra_detail::parse_civil("20 6-01-01", c));
  EXPECT_FALSE(opra_detail::parse_civil("2O26-01-01", c)); // letter O, not zero
  EXPECT_FALSE(opra_detail::parse_civil("+123-01-01", c));

  // And the ordinary date immediately either side of the change still parses.
  ASSERT_TRUE(opra_detail::parse_civil("0001-01-01", c));
  EXPECT_EQ(c.y, 1);
  EXPECT_EQ(c.m, 1u);
  EXPECT_EQ(c.d, 1u);
}

// Every ordinary date the loaders are given must still parse, and to the same
// value: the fix must not have narrowed anything real. Walks a whole year --
// every valid (month, day) of 2026 plus the leap day of 2024 -- and asserts both
// acceptance and the round-tripped fields.
TEST(OpraCivilDate, EveryRealDateStillParses) {
  const unsigned days_in_month[12] = {31u, 28u, 31u, 30u, 31u, 30u,
                                      31u, 31u, 30u, 31u, 30u, 31u};
  std::size_t accepted = 0;
  for (unsigned m = 1; m <= 12u; ++m) {
    for (unsigned d = 1; d <= days_in_month[m - 1u]; ++d) {
      char buf[11];
      std::snprintf(buf, sizeof buf, "2026-%02u-%02u", m, d);
      atx::vol::opra_detail::Civil c;
      ASSERT_TRUE(opra_detail::parse_civil(buf, c)) << buf;
      EXPECT_EQ(c.y, 2026);
      EXPECT_EQ(c.m, m);
      EXPECT_EQ(c.d, d);
      ++accepted;
    }
  }
  EXPECT_EQ(accepted, std::size_t{365});
}

// The operator-visible consequence, through the real spec gate of BOTH loaders:
// an impossible date is a top-level InvalidArgument, exactly like a malformed
// one, instead of quietly becoming a different window.
TEST(OpraBatch, MalformedSpec_ImpossibleCalendarDate_Rejected) {
  OpraBatchSpec spec;
  spec.symbols = {"XOM"};
  spec.root_dir = fs::temp_directory_path().string();

  spec.date_lo = "2026-02-31"; // does not exist; used to normalise to 2026-03-03
  spec.date_hi = "2026-03-05";
  const auto lo_bad = load_opra_daterange(spec);
  ASSERT_FALSE(lo_bad.has_value());
  EXPECT_EQ(lo_bad.error().code(), atx::vol::ErrorCode::InvalidArgument);

  spec.date_lo = "2026-03-01";
  spec.date_hi = "2026-04-31"; // April has 30 days
  const auto hi_bad = load_opra_daterange(spec);
  ASSERT_FALSE(hi_bad.has_value());
  EXPECT_EQ(hi_bad.error().code(), atx::vol::ErrorCode::InvalidArgument);

  // The hive loader shares the same kernel and the same gate.
  atx::vol::OpraHiveSpec hive;
  hive.root_dir = fs::temp_directory_path().string();
  hive.symbols = {"XOM"};
  hive.date_lo = "2026-02-30";
  hive.date_hi = "2026-03-05";
  const auto hive_bad = atx::vol::load_opra_hive(hive);
  ASSERT_FALSE(hive_bad.has_value());
  EXPECT_EQ(hive_bad.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(OpraBatch, MarketInputTableCanonicalizesSortsAndRejectsLookahead) {
  CorpusMarketInputCell xom = market_cell("2026-06-02", "xom", "2026-06-01T20:00:00Z");
  xom.spot_override = 111.0;
  xom.yc_pillar_t = {0.25, 1.0};
  xom.yc_pillar_r = {0.03, 0.04};
  CorpusMarketInputCell aapl = market_cell("2026-06-01", "aapl", "2026-06-01T12:00:00Z");
  aapl.spot_override = 250.0;

  auto table = CorpusMarketInputTable::create({xom, aapl});
  ASSERT_TRUE(table.has_value()) << table.error().to_string();
  ASSERT_EQ(table->cells().size(), 2u);
  EXPECT_EQ(table->cells()[0].date, "2026-06-01");
  EXPECT_EQ(table->cells()[0].symbol, "AAPL");
  EXPECT_EQ(table->cells()[1].symbol, "XOM");
  EXPECT_EQ(table->find("2026-06-02", "xom"), &table->cells()[1]);
  EXPECT_NE(table->fingerprint(), 0u);

  CorpusMarketInputCell future = market_cell("2026-06-01", "XOM", "2026-06-02T00:00:00Z");
  EXPECT_FALSE(CorpusMarketInputTable::create({std::move(future)}).has_value());
}

TEST(OpraBatch, MarketInputFingerprintChangesWithEconomicOrAsOfContent) {
  CorpusMarketInputCell base = market_cell("2026-06-01", "XOM", "2026-05-31T20:00:00Z");
  base.yc_pillar_t = {0.25, 1.0};
  base.yc_pillar_r = {0.03, 0.04};
  base.cash_divs = {{1782864000000000000LL, 0.50}};
  auto first = CorpusMarketInputTable::create({base});
  ASSERT_TRUE(first.has_value()) << first.error().to_string();

  CorpusMarketInputCell changed = base;
  changed.cash_divs[0].amount = 0.51;
  auto economic = CorpusMarketInputTable::create({changed});
  ASSERT_TRUE(economic.has_value()) << economic.error().to_string();
  EXPECT_NE(first->fingerprint(), economic->fingerprint());

  changed = base;
  changed.provenance.rates.as_of = "2026-05-30T20:00:00Z";
  auto as_of = CorpusMarketInputTable::create({changed});
  ASSERT_TRUE(as_of.has_value()) << as_of.error().to_string();
  EXPECT_NE(first->fingerprint(), as_of->fingerprint());
}

TEST(OpraBatch, PerCellInputsReachPanelAndCorpusBoard) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_batch_cell_inputs";
  fs::remove_all(root);
  write_pair(root / "XOM" / "2026-06-01.parquet", "XOM", "260918", 110.0, kXomFwd, true);

  CorpusMarketInputCell cell = market_cell("2026-06-01", "XOM", "2026-05-31T20:00:00Z");
  cell.spot_override = 123.45;
  cell.yc_pillar_t = {0.25, 1.0};
  cell.yc_pillar_r = {0.03, 0.04};
  cell.cash_divs = {{1782864000000000000LL, 0.50}};
  cell.fit_context.event_phase = atx::vol::EventPhase::PreAnnouncement;
  cell.fit_context.event_distance_days = 3u;
  cell.fit_context.htb = true;
  auto table = CorpusMarketInputTable::create({cell});
  ASSERT_TRUE(table.has_value()) << table.error().to_string();

  OpraBatchSpec spec;
  spec.symbols = {"XOM"};
  spec.date_lo = "2026-06-01";
  spec.date_hi = "2026-06-01";
  spec.root_dir = root.string();
  spec.market_inputs = *table;
  spec.missing_market_inputs = MissingMarketInputPolicy::Error;
  spec.provenance_mode = atx::vol::OpraProvenanceMode::Strict;
  auto loaded = load_opra_daterange(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  ASSERT_EQ(loaded->entries.size(), 1u);
  const OpraBatchEntry &entry = loaded->entries.front();
  ASSERT_TRUE(entry.panel.has_value()) << entry.panel.error().to_string();
  EXPECT_FALSE(entry.used_market_input_fallback);
  EXPECT_NE(entry.market_input_fingerprint, 0u);
  EXPECT_DOUBLE_EQ(entry.panel->implied_spot, 123.45);
  EXPECT_EQ(entry.panel->frame.yc_pillar_t, cell.yc_pillar_t);
  ASSERT_EQ(entry.panel->frame.divs.size(), 1u);
  EXPECT_DOUBLE_EQ(entry.panel->frame.divs[0].amount, 0.50);
  EXPECT_EQ(entry.panel->fit_context.event_phase, atx::vol::EventPhase::PreAnnouncement);
  ASSERT_TRUE(entry.panel->fit_context.htb.has_value());
  EXPECT_TRUE(*entry.panel->fit_context.htb);
  EXPECT_TRUE(entry.panel->provenance_complete);
  EXPECT_EQ(entry.panel->source_schema_version, 2u);
  EXPECT_NE(entry.panel->source_fingerprint, 0u);

  atx::vol::CorpusBoard board =
      atx::vol::corpus_board_from_opra(entry.date, entry.symbol, *entry.panel);
  EXPECT_EQ(board.date, "2026-06-01");
  EXPECT_EQ(board.symbol, "XOM");
  EXPECT_TRUE(board.source_provenance_complete);
  EXPECT_EQ(board.source_schema_version, 2u);
  EXPECT_EQ(board.source_fingerprint, entry.panel->source_fingerprint);
  EXPECT_EQ(board.market_input_fingerprint, entry.market_input_fingerprint);
  EXPECT_EQ(board.fit_context.event_phase, atx::vol::EventPhase::PreAnnouncement);
  ASSERT_EQ(board.env.cash_divs.size(), 1u);
  EXPECT_DOUBLE_EQ(board.env.cash_divs[0].amount, 0.50);
  EXPECT_DOUBLE_EQ(board.env.rate_at(0.5), market_env_from_frame(board.frame).rate_at(0.5));
  fs::remove_all(root);
}

TEST(OpraBatch, MissingCellPolicyCanQuarantineOrError) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_batch_missing_inputs";
  fs::remove_all(root);
  write_pair(root / "XOM" / "2026-06-01.parquet", "XOM", "260918", 110.0, kXomFwd);
  OpraBatchSpec spec;
  spec.symbols = {"XOM"};
  spec.date_lo = "2026-06-01";
  spec.date_hi = "2026-06-01";
  spec.root_dir = root.string();
  spec.missing_market_inputs = MissingMarketInputPolicy::Quarantine;
  auto quarantined = load_opra_daterange(spec);
  ASSERT_TRUE(quarantined.has_value()) << quarantined.error().to_string();
  ASSERT_EQ(quarantined->entries.size(), 1u);
  EXPECT_FALSE(quarantined->entries[0].panel.has_value());
  EXPECT_EQ(quarantined->entries[0].panel.error().code(), atx::vol::ErrorCode::Unavailable);

  spec.missing_market_inputs = MissingMarketInputPolicy::Error;
  auto failed = load_opra_daterange(spec);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code(), atx::vol::ErrorCode::Unavailable);
  fs::remove_all(root);
}

// ── market_env_from_frame: the term-curve bridge ────────────────────────────

TEST(OpraBatch, MarketEnvFromFrame_TermCurveInterpolatesShortRate) {
  // A materially-sloped curve bracketing a short front maturity.
  const std::vector<double> pt = {0.1, 2.0};
  const std::vector<double> pr = {0.02, 0.06};

  QuoteFrame frame;
  frame.spot = 100.0;
  frame.snapshot_ts_ns = 1780000000000000000LL;
  frame.yc_pillar_t = pt;
  frame.yc_pillar_r = pr;

  const MarketEnv env = market_env_from_frame(frame);
  EXPECT_DOUBLE_EQ(env.spot, 100.0);
  EXPECT_EQ(env.now_ns, frame.snapshot_ts_ns);
  ASSERT_GT(env.yield.size(), std::size_t{0}); // a real term curve was built

  // Independently-built reference curve: the env's rate_at must equal the
  // monotone-Hermite zero rate at each maturity, bit-for-bit.
  const auto yc = YieldCurve::create(std::span<const double>(pt), std::span<const double>(pr));
  ASSERT_TRUE(yc.has_value()) << yc.error().to_string();

  const double front_T = 0.25; // a single date's front expiry, ~3 months out
  EXPECT_DOUBLE_EQ(env.rate_at(front_T), yc->zero(front_T));

  // The interpolated short rate genuinely sits between the pillars (term
  // structure is live), NOT collapsed to a flat number.
  EXPECT_GT(env.rate_at(front_T), 0.02);
  EXPECT_LT(env.rate_at(front_T), 0.06);
}

TEST(OpraBatch, MarketEnvFromFrame_NoOrSinglePillarIsFlat) {
  // (A) No pillars -> flat 0.
  QuoteFrame f0;
  f0.spot = 100.0;
  f0.snapshot_ts_ns = 1780000000000000000LL;
  const MarketEnv e0 = market_env_from_frame(f0);
  EXPECT_EQ(e0.yield.size(), std::size_t{0}); // no curve
  EXPECT_DOUBLE_EQ(e0.rate_at(0.25), 0.0);
  EXPECT_DOUBLE_EQ(e0.spot, 100.0);

  // (B) A single pillar -> flat at that pillar's rate (a 1-pillar curve does NOT
  // interpolate flat, so it must be treated as the flat rate).
  QuoteFrame f1;
  f1.spot = 100.0;
  f1.snapshot_ts_ns = 1780000000000000000LL;
  f1.yc_pillar_t = {1.0};
  f1.yc_pillar_r = {0.05};
  const MarketEnv e1 = market_env_from_frame(f1);
  EXPECT_EQ(e1.yield.size(), std::size_t{0}); // still no curve
  EXPECT_DOUBLE_EQ(e1.rate_at(0.25), 0.05);   // the flat rate at any T
  EXPECT_DOUBLE_EQ(e1.rate_at(5.0), 0.05);
}

TEST(OpraBatch, TermRatesReachFitLiveQueryAndArchivedQuery) {
  atx::vol::SynthPanelSpec synth = atx::vol::make_spy_synthetic_spec("2026-06-17");
  auto panel = atx::vol::make_synthetic_american_panel(synth);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  auto curve = YieldCurve::create(std::vector<double>{0.01, 2.0}, std::vector<double>{0.02, 0.06});
  ASSERT_TRUE(curve.has_value()) << curve.error().to_string();
  MarketEnv env =
      MarketEnv::flat(synth.spot, synth.r, panel->frame.snapshot_ts_ns, panel->frame.divs);
  env.yield = *curve;
  auto chain = atx::vol::OptionChain::from_frame(panel->frame, env);
  ASSERT_TRUE(chain.has_value()) << chain.error().to_string();

  atx::vol::PricerConfig config;
  atx::vol::CurveConfig essvi;
  essvi.kind = atx::vol::VolCurveKind::Essvi;
  config.curve = essvi;
  config.use_correction_cache = false;
  atx::vol::PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain).has_value());
  const atx::vol::VolaSession &session = fitter.surface()->session();
  ASSERT_GE(session.expiries().size(), 2u);

  for (const atx::vol::SliceContext &expiry : session.expiries()) {
    EXPECT_DOUBLE_EQ(session.rate_at(expiry.T), env.rate_at(expiry.T));
  }
  const atx::vol::SliceContext &expiry = session.expiries().back();
  const double strike = expiry.forward;
  const double sigma = session.iv(strike, expiry.T);
  const auto expected = atx::vol::american_price(
      synth.spot, strike, expiry.T, sigma, env.rate_at(expiry.T), expiry.q_eff,
      atx::vol::Side::Call, session.inputs().deam.method, session.inputs().deam.al_opts);
  const auto live = session.fair_value(strike, expiry.T, atx::vol::Side::Call);
  ASSERT_TRUE(expected.has_value() && live.has_value());
  EXPECT_DOUBLE_EQ(*live, *expected);

  auto priced = session.to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  atx::vol::SurfaceArchiveItem item{"SPY", &*priced};
  atx::vol::SurfaceArchiveWriteOpts write;
  write.created_ts_ns = 1;
  auto bytes = atx::vol::write_surface_archive(
      std::span<const atx::vol::SurfaceArchiveItem>(&item, 1u), write);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto archive = atx::vol::SurfaceArchive::open(std::move(*bytes));
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();
  auto reloaded = archive->map_symbol("SPY");
  ASSERT_TRUE(reloaded.has_value()) << reloaded.error().to_string();
  EXPECT_NEAR(reloaded->rate_at(expiry.T), env.rate_at(expiry.T), 1.0e-14);
  const auto archived = reloaded->fair_value(strike, expiry.T, atx::vol::Side::Call);
  ASSERT_TRUE(archived.has_value()) << archived.error().to_string();
  EXPECT_NEAR(*archived, *live, 1.0e-11);
}

} // namespace
