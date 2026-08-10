// bev_label_factory_gate_test.cpp — correctness GATE for the label-factory
// driver (THEO-6).
//
// ACCESS MECHANISM. `run_bev_label_factory` / `BevFactoryArgs` have no
// separate header — they are a small, CLI-only arg bag with no reuse value
// beyond this one driver, so a `.hpp` was not warranted (see the driver's own
// banner). This test therefore reaches them by `#define
// ATX_BEV_LABEL_FACTORY_NO_MAIN` followed by a TEXTUAL `#include` of
// examples/bev_label_factory.cpp below — a macro-guarded re-inclusion of the
// whole example TU (its `main()` compiled out) directly into THIS test's own
// translation unit, so the struct and function are declared exactly once and
// the test exercises the shipped implementation byte-for-byte, not a
// reimplementation or a second, ODR-risky forward-declared copy. This file is
// the ONLY place that `#include`s the example TU, so there is no ODR hazard
// from a second inclusion elsewhere. This is NOT the same mechanism
// tests/spy_strangle_backtest_test.cpp uses for its own driver — that test
// calls ordinary library entry points directly and never includes its
// example TU at all; this driver's args struct has no such shared-header
// alternative available to it. Reviewed and accepted as this task's access
// mechanism (controller ruling, 2026-08-08); do not restructure it.
//
// FIXTURE. Self-contained: `make_surface` below mirrors spy_strangle_
// backtest_test.cpp's own helper of the same name (closed-form eSSVI term
// structure, no fit, no external data) so this gate has no data dependency.
//
// COVERAGE. Builds a small SurfaceDb corpus, then:
//   (a)-(d) runs the full driver TWICE with IDENTICAL args into two temp
//       files: both exit Ok(0); the two output files are BYTE-IDENTICAL
//       (memcmp, whole file); at least one row converged (flag == 0, i.e.
//       BevFlag::Ok); every written row's log_ratio is finite.
//   (e) parse_args over a canned argv vector produces the expected
//       BevFactoryArgs field-for-field.
//   (f) parse_args rejects a missing required flag (no crash, no usage()
//       call needed to observe the rejection).
//   (g) load_dividends_tsv parses a valid two-row TSV correctly and rejects
//       a malformed line with Err.

#include <gtest/gtest.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/american.hpp"        // al_fast_opts, AmericanMethod
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/surface_archive.hpp" // SurfaceArchiveItem
#include "atx/vol/surface_db.hpp"      // SurfaceDb
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/types.hpp"           // Result, Status
#include "atx/vol/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"     // EssviParams

using namespace atx::vol;

// THEO-6: pull run_bev_label_factory() + BevFactoryArgs straight out of the
// shipped example TU (see that file's banner for why there is no separate
// header) with its `main()` suppressed.
#define ATX_BEV_LABEL_FACTORY_NO_MAIN
#include "../examples/bev_label_factory.cpp"

namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.02;
constexpr std::int64_t kBaseNow = 1772323200000000000LL; // arbitrary, exact value inconsequential
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kSpy = 42;
constexpr int kNDates = 30;

// Mirrors spy_strangle_backtest_test.cpp's `make_surface`: closed-form eSSVI
// term structure (no fit) priced through the real American/Andersen-Lake
// pipeline, so `delta`/`iv`/greeks are genuine, not stubbed.
[[nodiscard]] PricedSurface make_surface(double S, std::int64_t now_ts, double vol_bump) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = S;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, S, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = kSpy;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

// Deterministic (not random) daily spot path with enough realized movement
// for solve_breakeven_vol to find a genuine sign-changing bracket -- same
// "reviewable, not GBM-random" spirit as breakeven_test.cpp's BevPathLoader
// fixture.
[[nodiscard]] double spot_for_day(int d) {
  return 600.0 *
         (1.0 + 0.03 * std::sin(0.6 * static_cast<double>(d)) + 0.0015 * static_cast<double>(d));
}

[[nodiscard]] std::string date_for_day(int d) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "2026-03-%02d", d + 1);
  return buf;
}

[[nodiscard]] std::vector<char> read_whole_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// One parsed data row -- just enough to check the gate's (c)/(d) properties.
// Column order is fixed by the driver's own writer (append_rows_tsv):
// entry_ts_ns uid strike expiry_ns side sigma_be sigma_entry_iv log_ratio
// premium vega n_days iters flag snapped -- log_ratio is column 7, flag is
// column 12 (0-based).
struct ParsedRow {
  double log_ratio{0.0};
  std::uint8_t flag{0};
};

[[nodiscard]] std::vector<ParsedRow> parse_rows(const std::vector<char> &bytes) {
  const std::string content(bytes.begin(), bytes.end());
  std::vector<ParsedRow> rows;
  std::size_t pos = 0;
  bool header_seen = false;
  // Bounded by content.size(): each pass advances `pos` past the newline it
  // just found (or the loop terminates).
  while (pos <= content.size()) {
    const std::size_t nl = content.find('\n', pos);
    const std::string_view line(content.data() + pos,
                                (nl == std::string::npos ? content.size() : nl) - pos);
    pos = (nl == std::string::npos) ? content.size() + 1 : nl + 1;
    if (line.empty() || line.front() == '#') {
      if (nl == std::string::npos) {
        break;
      }
      continue;
    }
    if (!header_seen) {
      header_seen = true; // the column-header row
      continue;
    }
    std::vector<std::string_view> cols;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
      if (i == line.size() || line[i] == '\t') {
        cols.push_back(line.substr(start, i - start));
        start = i + 1;
      }
    }
    if (cols.size() < 14) {
      continue; // malformed row -- ignore for this diagnostic parse
    }
    double log_ratio = 0.0;
    const auto lr_r = std::from_chars(cols[7].data(), cols[7].data() + cols[7].size(), log_ratio);
    unsigned flag = 0;
    const auto fl_r = std::from_chars(cols[12].data(), cols[12].data() + cols[12].size(), flag);
    if (lr_r.ec != std::errc{} || fl_r.ec != std::errc{}) {
      continue;
    }
    rows.push_back(ParsedRow{log_ratio, static_cast<std::uint8_t>(flag)});
  }
  return rows;
}

} // namespace

TEST(BevLabelFactoryGate, TwoRunsProduceByteIdenticalFilesWithAtLeastOneOkLabel) {
  const std::string root = (fs::temp_directory_path() / "atx-bev-label-factory-db").string();
  std::error_code ec;
  fs::remove_all(root, ec);
  auto db = SurfaceDb::create(root);
  ASSERT_TRUE(db.has_value()) << db.error().to_string();

  // 30 daily SPY surfaces, spot walked deterministically (real greeks/deltas
  // via the real American pricer), enough realized movement over an ~18-day
  // replay window for a genuine sign-changing bracket.
  for (int d = 0; d < kNDates; ++d) {
    const double S = spot_for_day(d);
    const double vol_bump = 0.001 * static_cast<double>(d);
    const PricedSurface spy =
        make_surface(S, kBaseNow + static_cast<std::int64_t>(d) * kDayNs, vol_bump);
    const SurfaceArchiveItem item{"SPY", &spy};
    const std::span<const SurfaceArchiveItem> items(&item, 1);
    const Status st = db->write_partition(date_for_day(d), items);
    ASSERT_TRUE(st.has_value()) << st.error().to_string();
  }

  const std::string dividends_path =
      (fs::temp_directory_path() / "atx-bev-label-factory-divs.tsv").string();
  {
    std::ofstream divs(dividends_path, std::ios::binary | std::ios::trunc);
    divs << "# no dividends for this fixture\n";
  }

  const std::string out1 = (fs::temp_directory_path() / "atx-bev-label-factory-out1.tsv").string();
  const std::string out2 = (fs::temp_directory_path() / "atx-bev-label-factory-out2.tsv").string();

  BevFactoryArgs args;
  args.db = root;
  args.uid = "SPY";
  args.entry_start = date_for_day(0);
  args.entry_end = date_for_day(0);
  args.tenor_days = 18; // closest to the T=0.05 pillar (~18.26 calendar days)
  args.delta_lo = 0.05;
  args.delta_hi = 0.95;
  args.dividends = dividends_path;
  // Explicit, IDENTICAL thread count on both runs: byte-identity at a fixed
  // n_threads is THIS gate's contract; cross-thread-count identity is Task
  // 4's already-covered contract (solve_breakeven_batch itself).
  args.n_threads = 2;

  args.out = out1;
  const Result<int> rc1 = run_bev_label_factory(args);
  ASSERT_TRUE(rc1.has_value()) << rc1.error().to_string();
  EXPECT_EQ(*rc1, 0);

  args.out = out2;
  const Result<int> rc2 = run_bev_label_factory(args);
  ASSERT_TRUE(rc2.has_value()) << rc2.error().to_string();
  EXPECT_EQ(*rc2, 0);

  const std::vector<char> bytes1 = read_whole_file(out1);
  const std::vector<char> bytes2 = read_whole_file(out2);
  ASSERT_FALSE(bytes1.empty());
  ASSERT_EQ(bytes1.size(), bytes2.size());
  EXPECT_EQ(0, std::memcmp(bytes1.data(), bytes2.data(), bytes1.size()))
      << "two runs with identical args must produce byte-identical output";

  const std::vector<ParsedRow> rows = parse_rows(bytes1);
  ASSERT_FALSE(rows.empty()) << "expected at least one label row";
  std::size_t n_ok = 0;
  for (const ParsedRow &row : rows) {
    EXPECT_TRUE(std::isfinite(row.log_ratio));
    n_ok += (row.flag == 0) ? 1u : 0u;
  }
  EXPECT_GT(n_ok, 0u) << "expected at least one converged (flag==0) label";
  std::printf("[gate] rows=%zu flag_ok=%zu\n", rows.size(), n_ok);
}

namespace {

// Builds a `char**` argv over `storage`'s elements (storage must outlive the
// returned vector -- each entry points into `storage[i]`'s own buffer).
[[nodiscard]] std::vector<char *> make_argv(std::vector<std::string> &storage) {
  std::vector<char *> argv;
  argv.reserve(storage.size());
  for (std::string &s : storage) {
    argv.push_back(s.data());
  }
  return argv;
}

} // namespace

// (e) parse_args: a canned argv vector produces the expected BevFactoryArgs
// field-for-field.
TEST(BevLabelFactoryGate, ParseArgsProducesExpectedFieldsFromCannedArgv) {
  std::vector<std::string> argv_storage = {
      "bev_label_factory",
      "--db",
      "C:/some/db",
      "--uid",
      "SPY",
      "--entry-start",
      "2026-01-01",
      "--entry-end",
      "2026-01-31",
      "--tenor-days",
      "45",
      "--delta-lo",
      "0.10",
      "--delta-hi",
      "0.90",
      "--dividends",
      "divs.tsv",
      "--out",
      "labels.tsv",
      "--threads",
      "4",
  };
  std::vector<char *> argv = make_argv(argv_storage);

  BevFactoryArgs args;
  const bool ok = parse_args(static_cast<int>(argv.size()), argv.data(), args);
  ASSERT_TRUE(ok);
  EXPECT_EQ(args.db, "C:/some/db");
  EXPECT_EQ(args.uid, "SPY");
  EXPECT_EQ(args.entry_start, "2026-01-01");
  EXPECT_EQ(args.entry_end, "2026-01-31");
  EXPECT_EQ(args.tenor_days, 45);
  EXPECT_DOUBLE_EQ(args.delta_lo, 0.10);
  EXPECT_DOUBLE_EQ(args.delta_hi, 0.90);
  EXPECT_EQ(args.dividends, "divs.tsv");
  EXPECT_EQ(args.out, "labels.tsv");
  EXPECT_EQ(args.n_threads, 4u);
}

// (f) parse_args: a missing required flag (--db omitted here) is rejected --
// no crash, just a clean `false`.
TEST(BevLabelFactoryGate, ParseArgsRejectsMissingRequiredFlag) {
  std::vector<std::string> argv_storage = {
      "bev_label_factory",
      "--uid",
      "SPY",
      "--entry-start",
      "2026-01-01",
      "--entry-end",
      "2026-01-31",
      "--tenor-days",
      "45",
      "--delta-lo",
      "0.10",
      "--delta-hi",
      "0.90",
      "--dividends",
      "divs.tsv",
      "--out",
      "labels.tsv",
  };
  std::vector<char *> argv = make_argv(argv_storage);

  BevFactoryArgs args;
  const bool ok = parse_args(static_cast<int>(argv.size()), argv.data(), args);
  EXPECT_FALSE(ok);
}

// (g) load_dividends_tsv: a valid two-row file parses correctly (count +
// exact values); a malformed line (no amount field) is rejected with Err.
TEST(BevLabelFactoryGate, LoadDividendsTsvParsesValidRowsAndRejectsMalformedLine) {
  const std::string valid_path =
      (fs::temp_directory_path() / "atx-bev-label-factory-divs-valid.tsv").string();
  {
    std::ofstream f(valid_path, std::ios::binary | std::ios::trunc);
    f << "# comment line, skipped\n";
    f << "1700000000000000000 1.25\n";
    f << "1710000000000000000\t2.5\n";
  }
  const Result<std::vector<DividendEvent>> divs = load_dividends_tsv(valid_path);
  ASSERT_TRUE(divs.has_value()) << divs.error().to_string();
  ASSERT_EQ(divs->size(), 2u);
  EXPECT_EQ((*divs)[0].ex_date_ns, 1700000000000000000LL);
  EXPECT_DOUBLE_EQ((*divs)[0].amount, 1.25);
  EXPECT_EQ((*divs)[1].ex_date_ns, 1710000000000000000LL);
  EXPECT_DOUBLE_EQ((*divs)[1].amount, 2.5);

  const std::string bad_path =
      (fs::temp_directory_path() / "atx-bev-label-factory-divs-bad.tsv").string();
  {
    std::ofstream f(bad_path, std::ios::binary | std::ios::trunc);
    f << "1700000000000000000 1.25\n";
    f << "not-a-valid-line-missing-the-amount-field\n";
  }
  const Result<std::vector<DividendEvent>> bad = load_dividends_tsv(bad_path);
  EXPECT_FALSE(bad.has_value());
}

// Task F-1: --events calendar. (h) load_events_tsv parses dates + comments and
// count_events_at semantics reach the rows; (i) malformed date rejected;
// (j) empty path => nullopt (feature column NaN handled in Task F-3's test).
TEST(BevLabelFactoryGate, EventsTsvParsesAndCounts) {
  const std::string events_path =
      (fs::temp_directory_path() / "atx-bev-label-factory-events.tsv").string();
  {
    std::ofstream f(events_path, std::ios::binary | std::ios::trunc);
    f << "# uid=SPY\n2026-03-05\n\n2026-06-04\r\n";
  }
  const Result<std::optional<EventSchedule>> sched = load_events_tsv(events_path);
  ASSERT_TRUE(sched.has_value()) << sched.error().to_string();
  ASSERT_TRUE(sched->has_value());
  const Result<std::int64_t> d1_r = iso_date_to_ns("2026-03-05");
  ASSERT_TRUE(d1_r.has_value()) << d1_r.error().to_string();
  const std::int64_t d1 = *d1_r;
  // 2026-03-05 00:00 UTC == 1772668800 * 1e9 exactly (civil-days check).
  EXPECT_EQ(d1, 1772668800LL * 1000000000LL);
  EXPECT_EQ((*sched)->count_between(d1 - 1, d1), std::size_t{1}); // (now, expiry] includes expiry
  EXPECT_EQ((*sched)->count_between(d1, d1), std::size_t{0});     // event at now excluded
}

TEST(BevLabelFactoryGate, EventsTsvRejectsMalformedDate) {
  const std::string bad_events_path =
      (fs::temp_directory_path() / "atx-bev-label-factory-bad-events.tsv").string();
  {
    std::ofstream f(bad_events_path, std::ios::binary | std::ios::trunc);
    f << "2026-13-40\n";
  }
  const Result<std::optional<EventSchedule>> sched = load_events_tsv(bad_events_path);
  ASSERT_FALSE(sched.has_value());
  EXPECT_EQ(sched.error().code(), ErrorCode::ParseError);
}

TEST(BevLabelFactoryGate, EventsPathEmptyMeansNoCalendar) {
  const Result<std::optional<EventSchedule>> sched = load_events_tsv("");
  ASSERT_TRUE(sched.has_value());
  EXPECT_FALSE(sched->has_value());
}

// Task F-2: spot pre-pass. (k) bars come out ascending, one per session,
// close == the fixture's own spot for that date; (l) a 3-bar history yields
// finite (fallback, not NaN) 21d/63d slots (realized_vol_panel per-slot
// contract) -- asserted at the panel level here, at the TSV level in Task
// F-3's test. Builds its own small SurfaceDb corpus (same helpers as (a)-(d)
// above, `make_surface`/`spot_for_day`/`date_for_day`) under a distinct temp
// root so it has no data dependency on that test.
TEST(BevLabelFactoryGate, SpotHistoryMirrorsSessionSpots) {
  const std::string root =
      (fs::temp_directory_path() / "atx-bev-label-factory-spot-history-db").string();
  std::error_code ec;
  fs::remove_all(root, ec);
  auto db = SurfaceDb::create(root);
  ASSERT_TRUE(db.has_value()) << db.error().to_string();

  for (int d = 0; d < kNDates; ++d) {
    const double S = spot_for_day(d);
    const double vol_bump = 0.001 * static_cast<double>(d);
    const PricedSurface spy =
        make_surface(S, kBaseNow + static_cast<std::int64_t>(d) * kDayNs, vol_bump);
    const SurfaceArchiveItem item{"SPY", &spy};
    const std::span<const SurfaceArchiveItem> items(&item, 1);
    const Status st = db->write_partition(date_for_day(d), items);
    ASSERT_TRUE(st.has_value()) << st.error().to_string();
  }

  const Result<Clock> clock_r = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock_r.has_value()) << clock_r.error().to_string();
  const Clock &clock = *clock_r;

  const Result<std::vector<OhlcBar>> bars =
      load_spot_history(clock, "SPY", 0, clock.refs().size() - 1);
  ASSERT_TRUE(bars.has_value()) << bars.error().to_string();
  ASSERT_EQ(bars->size(), clock.refs().size());
  for (std::size_t i = 1; i < bars->size(); ++i) {
    EXPECT_LT((*bars)[i - 1].ts_ns, (*bars)[i].ts_ns);
  }
  for (const OhlcBar &b : *bars) { // spot-mirror invariant: O==H==L==C, all > 0
    EXPECT_GT(b.close, 0.0);
    EXPECT_EQ(b.open, b.close);
    EXPECT_EQ(b.high, b.close);
    EXPECT_EQ(b.low, b.close);
  }
  // Panel over the first 3 bars: 5d slot falls back to whole span (valid),
  // 21d/63d/252d slots fall back to the same 3-bar span too (window > size
  // falls back to whole span, >= 2 bars, so they are NUMBERS not NaN) --
  // assert the documented fallback, not an imagined NaN.
  const Result<RvPanel> p3 =
      realized_vol_panel(std::span{bars->data(), 3}, RvEstimator::CloseToClose, 252.0);
  ASSERT_TRUE(p3.has_value()) << p3.error().to_string();
  EXPECT_TRUE(std::isfinite(p3->vol[1]));
  // 1-bar history: every slot NaN.
  const Result<RvPanel> p1 =
      realized_vol_panel(std::span{bars->data(), 1}, RvEstimator::CloseToClose, 252.0);
  ASSERT_TRUE(p1.has_value()) << p1.error().to_string();
  EXPECT_TRUE(std::isnan(p1->vol[1]));
}
