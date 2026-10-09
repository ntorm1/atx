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

#include <bit>
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

#include "atx/vol/api/pricing/american.hpp"        // al_fast_opts, AmericanMethod
#include "atx/vol/api/backtest/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/api/storage/surface_archive.hpp" // SurfaceArchiveItem
#include "atx/vol/api/storage/surface_db.hpp"      // SurfaceDb
#include "atx/vol/api/fitting/surface_parity.hpp"  // SliceContext
#include "atx/vol/api/core/types.hpp"           // Result, Status
#include "atx/vol/api/fitting/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/api/fitting/vol_surface.hpp"     // EssviParams

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
      // A canned argv TOKEN, not a lookup: parse_args only copies --db into
      // args.db and checks it non-empty, so this stays a relative placeholder
      // rather than an absolute that reads as a path into another checkout.
      "some/db",
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
  EXPECT_EQ(args.db, "some/db");
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

namespace {

// Splits one TSV line on '\t'. Local to this test (not shared with
// `parse_rows` above, which only extracts two columns for (a)-(d)'s narrower
// needs) -- the ~5-line helper the brief anticipated.
[[nodiscard]] std::vector<std::string_view> split_tsv_line(std::string_view line) {
  std::vector<std::string_view> cols;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '\t') {
      cols.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  return cols;
}

// Reads `path`, splits it into the column-header line and every data row's
// columns (comment lines skipped, same shape as `parse_rows`'s own walk but
// keeping every column rather than just log_ratio/flag). `content_out` must
// outlive the returned string_views (they point into it).
void parse_full_tsv(const std::string &path, std::string &content_out, std::string_view &header,
                    std::vector<std::vector<std::string_view>> &data_rows) {
  const std::vector<char> bytes = read_whole_file(path);
  content_out.assign(bytes.begin(), bytes.end());
  std::size_t pos = 0;
  bool header_seen = false;
  // Bounded by content_out.size(): each pass advances `pos` past the newline
  // it just found (or the loop terminates).
  while (pos <= content_out.size()) {
    const std::size_t nl = content_out.find('\n', pos);
    const std::string_view line(content_out.data() + pos,
                                (nl == std::string::npos ? content_out.size() : nl) - pos);
    pos = (nl == std::string::npos) ? content_out.size() + 1 : nl + 1;
    if (line.empty() || line.front() == '#') {
      if (nl == std::string::npos) {
        break;
      }
      continue;
    }
    if (!header_seen) {
      header_seen = true;
      header = line;
      continue;
    }
    data_rows.push_back(split_tsv_line(line));
  }
}

} // namespace

// Task F-3: (m) header carries the schema block, names and ORDER frozen to
// kFairVolFeatureSchemaV1; (n) per-row spot-check: log_moneyness/tenor/
// market_vol/delta_abs recomputed from the fixture surface match the emitted
// values; iv_minus_rv == market_vol - rv_21d exactly; (o) no --events =>
// n_events_to_expiry column is "nan"; with a one-event calendar between entry
// and expiry it is "1"; (p) determinism gate still holds with all new columns
// (covered by re-running (a)-(d) unchanged -- parse_rows's `cols.size() < 14`
// check already tolerates the eight appended columns, so no fix was needed
// there).
TEST(BevLabelFactoryGate, FeatureBlockHeaderAndValues) {
  static_assert(kFairVolFeatureCount == 8); // schema drift tripwire

  const std::string root =
      (fs::temp_directory_path() / "atx-bev-label-factory-feature-block-db").string();
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

  const std::string dividends_path =
      (fs::temp_directory_path() / "atx-bev-label-factory-feature-block-divs.tsv").string();
  {
    std::ofstream divs(dividends_path, std::ios::binary | std::ios::trunc);
    divs << "# no dividends for this fixture\n";
  }

  // Entry day 10 (not day 0, unlike (a)-(d)): the spot pre-pass then has 11
  // trailing bars, enough for realized_vol_panel's 21d/63d slots to come out
  // FINITE (fallback-to-whole-span, per (l)'s own documented contract) rather
  // than NaN -- this test needs a converged row with a non-degenerate
  // iv_minus_rv to exercise the real subtraction, not just NaN propagation.
  constexpr int kEntryDay = 10;
  BevFactoryArgs args;
  args.db = root;
  args.uid = "SPY";
  args.entry_start = date_for_day(kEntryDay);
  args.entry_end = date_for_day(kEntryDay);
  args.tenor_days = 18; // closest to the T=0.05 pillar (~18.26 calendar days)
  args.delta_lo = 0.05;
  args.delta_hi = 0.95;
  args.dividends = dividends_path;
  args.n_threads = 2;

  // (o) part 1: no --events => n_events_to_expiry column is the literal
  // "nan" for every row.
  const std::string out_no_events =
      (fs::temp_directory_path() / "atx-bev-label-factory-feature-block-noev.tsv").string();
  args.events.clear();
  args.out = out_no_events;
  const Result<int> rc_no_events = run_bev_label_factory(args);
  ASSERT_TRUE(rc_no_events.has_value()) << rc_no_events.error().to_string();
  EXPECT_EQ(*rc_no_events, 0);

  std::string content;
  std::string_view header;
  std::vector<std::vector<std::string_view>> rows;
  parse_full_tsv(out_no_events, content, header, rows);

  // (m) header tail: exact names, exact order.
  const std::string expected_tail =
      "log_moneyness\ttenor_years\tmarket_vol\trv_21d\trv_63d\tiv_minus_rv\t"
      "n_events_to_expiry\tdelta_abs";
  EXPECT_NE(header.find(expected_tail), std::string_view::npos) << header;

  ASSERT_FALSE(rows.empty());
  const std::vector<std::string_view> *converged = nullptr;
  for (const auto &row : rows) {
    ASSERT_GE(row.size(), 22u) << "expected 14 existing + 8 schema columns";
    if (row[12] == "0") { // flag column, 0-based -- BevFlag::Ok
      converged = &row;
      break;
    }
  }
  ASSERT_NE(converged, nullptr) << "expected at least one converged (flag==0) row";

  // (o) part 1 cont'd: n_events_to_expiry (column 20) is literal "nan".
  EXPECT_EQ((*converged)[20], "nan");

  const auto parse_col = [](std::string_view col) {
    double v = 0.0;
    std::from_chars(col.data(), col.data() + col.size(), v);
    return v;
  };
  const double strike = parse_col((*converged)[2]);
  const double sigma_entry_iv = parse_col((*converged)[6]);
  const double log_moneyness = parse_col((*converged)[14]);
  const double tenor_years = parse_col((*converged)[15]);
  const double market_vol = parse_col((*converged)[16]);
  const double rv_21d = parse_col((*converged)[17]);
  const double iv_minus_rv = parse_col((*converged)[19]);
  const double delta_abs = parse_col((*converged)[21]);

  // (n) spot-check against the fixture surface for kEntryDay, reconstructed
  // exactly as the driver itself would have (same S/now_ts/vol_bump).
  ASSERT_FALSE(std::isnan(rv_21d)) << "kEntryDay was chosen to make rv_21d finite";
  const PricedSurface fixture = make_surface(
      spot_for_day(kEntryDay), kBaseNow + static_cast<std::int64_t>(kEntryDay) * kDayNs,
      0.001 * static_cast<double>(kEntryDay));
  const double forward = fixture.forward_at(tenor_years);
  EXPECT_NEAR(log_moneyness, std::log(strike / forward), 1e-12);
  EXPECT_DOUBLE_EQ(market_vol, sigma_entry_iv); // same surf.iv(K,T) read, bit-equal
  EXPECT_DOUBLE_EQ(iv_minus_rv, market_vol - rv_21d);
  EXPECT_GE(delta_abs, args.delta_lo);
  EXPECT_LE(delta_abs, args.delta_hi);

  // (o) part 2: a one-event calendar strictly between entry and expiry =>
  // n_events_to_expiry is "1".
  const std::string events_path =
      (fs::temp_directory_path() / "atx-bev-label-factory-feature-block-events.tsv").string();
  {
    std::ofstream f(events_path, std::ios::binary | std::ios::trunc);
    f << date_for_day(kEntryDay + 10) << "\n"; // well inside (entry, entry+T]
  }
  const std::string out_with_events =
      (fs::temp_directory_path() / "atx-bev-label-factory-feature-block-withev.tsv").string();
  args.events = events_path;
  args.out = out_with_events;
  const Result<int> rc_with_events = run_bev_label_factory(args);
  ASSERT_TRUE(rc_with_events.has_value()) << rc_with_events.error().to_string();
  EXPECT_EQ(*rc_with_events, 0);

  std::string content2;
  std::string_view header2;
  std::vector<std::vector<std::string_view>> rows2;
  parse_full_tsv(out_with_events, content2, header2, rows2);
  ASSERT_FALSE(rows2.empty());
  bool saw_one_event_row = false;
  for (const auto &row : rows2) {
    ASSERT_GE(row.size(), 22u);
    if (row[20] == "1") {
      saw_one_event_row = true;
      break;
    }
  }
  EXPECT_TRUE(saw_one_event_row) << "expected n_events_to_expiry==1 for the one-event calendar";
}

// ════════════════════════════════════════════════════════════════════════
// VrpPanel — gate for the --vrp-panel mode (vrp-ml round 1, lane vrp-panel).
// Frozen contract vrp_panel_v1: analytics/vrp_panel.hpp (reached through the
// example TU included above). Series-level tests drive `build_vrp_rows`
// directly (pure math, hand-computable); corpus-level tests drive
// `run_vrp_panel` end-to-end over the same self-contained eSSVI fixture the
// BevLabelFactoryGate tests use.
// ════════════════════════════════════════════════════════════════════════

namespace {

// vrp_panel_v1 column indices (kVrpPanelColumnsV1 order) used by the
// file-parsing assertions below.
constexpr std::size_t kColIvFair21 = 4;
constexpr std::size_t kColIvFair63 = 5;
constexpr std::size_t kColTermSlope = 12;

// NaN-safe byte-identity for doubles (EXPECT_EQ fails on NaN == NaN).
[[nodiscard]] bool same_bits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Synthetic per-session series for the pure row-builder tests: deterministic
// (reviewable, not random) spot path with genuine movement, slowly varying
// iv marks, every session valid.
[[nodiscard]] VrpSeries make_vrp_series(int n) {
  VrpSeries s;
  for (int i = 0; i < n; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "d%04d", i);
    s.dates.push_back(buf);
    s.ts_ns.push_back(kBaseNow + static_cast<std::int64_t>(i) * kDayNs);
    const double di = static_cast<double>(i);
    s.spot.push_back(500.0 * (1.0 + 0.02 * std::sin(0.7 * di) + 0.001 * di));
    s.iv21.push_back(0.20 + 0.01 * std::sin(0.3 * di));
    s.iv63.push_back(0.215 + 0.01 * std::sin(0.3 * di));
    // Deliberately BELOW iv21: on a skewed smile the ATM-forward read sits
    // under the strip, so a test that mixes the two legs up fails loudly.
    s.iv_atmf21.push_back(0.185 + 0.01 * std::sin(0.3 * di));
  }
  return s;
}

// Same closed-form eSSVI fixture as `make_surface` above, but with the
// fitted pillars STOPPING at T = 0.20: 21/252 (~0.083) stays inside the
// fitted range while 63/252 (0.25) falls OUTSIDE it, so the 63d strip is
// OutOfRange by construction (gate for the f4-NaN-without-drop rule).
[[nodiscard]] PricedSurface make_surface_narrow(double S, std::int64_t now_ts, double vol_bump) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20};
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

// Write a fresh SurfaceDb corpus of SPY surfaces for days
// [first_day, first_day + n_days) under `root` (same deterministic
// spot/vol-bump walk as the BevLabelFactoryGate corpora).
//
// `split_day >= 0` divides the spot by 10 from that day onward, planting an
// UNADJUSTED 10-for-1 split exactly as a raw SurfaceDb spot series carries
// one. The surface's own vol is untouched — the corruption is in the spot
// mirror the forward-RV window reads, which is the defect being gated.
void write_vrp_corpus(const std::string &root, int first_day, int n_days, bool narrow_pillars,
                      int split_day = -1) {
  std::error_code ec;
  fs::remove_all(root, ec);
  auto db = SurfaceDb::create(root);
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  for (int d = first_day; d < first_day + n_days; ++d) {
    const double S = (split_day >= 0 && d >= split_day) ? spot_for_day(d) * 0.1 : spot_for_day(d);
    const double vol_bump = 0.001 * static_cast<double>(d);
    const std::int64_t ts = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const PricedSurface spy =
        narrow_pillars ? make_surface_narrow(S, ts, vol_bump) : make_surface(S, ts, vol_bump);
    const SurfaceArchiveItem item{"SPY", &spy};
    const std::span<const SurfaceArchiveItem> items(&item, 1);
    const Status st = db->write_partition(date_for_day(d), items);
    ASSERT_TRUE(st.has_value()) << st.error().to_string();
  }
}

} // namespace

// CLI surface: repeated --db roots and --uid symbols collect in order; the
// mode flag itself is skipped; missing --out rejects.
TEST(VrpPanel, CliParseCollectsRepeatedRootsAndSymbols) {
  std::vector<std::string> argv_storage = {
      "bev_label_factory", "--vrp-panel", "--db",  "rootA",       "--db",
      "rootB",             "--uid",       "SPY",   "--uid",       "AAPL",
      "--entry-start",     "2019-01-01",  "--entry-end", "2019-12-31",
      "--out",             "panel.tsv",
  };
  std::vector<char *> argv = make_argv(argv_storage);
  VrpPanelConfig cfg;
  ASSERT_TRUE(parse_vrp_panel_args(static_cast<int>(argv.size()), argv.data(), cfg));
  EXPECT_EQ(cfg.db_roots, (std::vector<std::string>{"rootA", "rootB"}));
  EXPECT_EQ(cfg.symbols, (std::vector<std::string>{"SPY", "AAPL"}));
  EXPECT_EQ(cfg.entry_start, "2019-01-01");
  EXPECT_EQ(cfg.entry_end, "2019-12-31");
  EXPECT_EQ(cfg.out, "panel.tsv");

  std::vector<std::string> bad_storage = {"bev_label_factory", "--vrp-panel", "--db", "rootA"};
  std::vector<char *> bad_argv = make_argv(bad_storage);
  VrpPanelConfig bad_cfg;
  EXPECT_FALSE(parse_vrp_panel_args(static_cast<int>(bad_argv.size()), bad_argv.data(), bad_cfg));
}

// Done-criterion (2): label = (rv_fwd^2 - iv_fair^2) * (21/252) to 1e-12 on
// a hand-computed fixture, with rv_fwd the annualized c2c vol over the
// 21-bar forward span — plus spot checks of the trailing feature formulas.
TEST(VrpPanel, LabelArithmeticMatchesHandComputedFixture) {
  const VrpSeries s = make_vrp_series(60);
  VrpPanelCounters c;
  const Result<std::vector<VrpPanelRow>> rows_r = build_vrp_rows(s, c, VrpPanelSchema::V2);
  ASSERT_TRUE(rows_r.has_value()) << rows_r.error().to_string();
  const std::vector<VrpPanelRow> &rows = *rows_r;
  ASSERT_EQ(rows.size(), 60u);

  const std::size_t t = 25;
  // Forward leg: realized_vol(CloseToClose) over bars t+1..t+21 == the 20
  // c2c return terms r_{t+2}..r_{t+21}, mean-of-squares, annualized by 252.
  double sum = 0.0;
  for (std::size_t j = t + 2; j <= t + 21; ++j) {
    const double r = std::log(s.spot[j] / s.spot[j - 1]);
    sum += r * r;
  }
  const double rv = std::sqrt(sum / 20.0 * 252.0);
  const double iv = s.iv21[t];
  EXPECT_NEAR(rows[t].rv_fwd_21d, rv, 1e-12);
  EXPECT_NEAR(rows[t].label, (rv * rv - iv * iv) * (21.0 / 252.0), 1e-12);

  // Trailing formula spot checks at the same t.
  const double r1 = std::log(s.spot[t] / s.spot[t - 1]);
  EXPECT_NEAR(rows[t].f0_log_rv1, std::log(std::max(252.0 * r1 * r1, 1e-8)), 1e-12);
  double sum21 = 0.0;
  for (std::size_t j = t - 20; j <= t; ++j) {
    const double r = std::log(s.spot[j] / s.spot[j - 1]);
    sum21 += r * r;
  }
  const double var21 = sum21 / 21.0 * 252.0; // trailing 21-session ann c2c variance
  EXPECT_NEAR(rows[t].f2_log_rv21, std::log(var21), 1e-12);
  EXPECT_NEAR(rows[t].f3_iv_level, std::log(iv * iv), 1e-12);
  EXPECT_NEAR(rows[t].f4_term_slope, s.iv63[t] - iv, 1e-15);
  EXPECT_NEAR(rows[t].f5_hv_iv_gap, std::log(std::sqrt(var21) / iv), 1e-12);
  EXPECT_NEAR(rows[t].f6_vrp_lag, iv * iv - var21, 1e-12);
  EXPECT_NEAR(rows[t].f7_ret_21d, std::log(s.spot[t] / s.spot[t - 21]), 1e-15);
}

// ── v4: the emitted axis IS the bar axis ─────────────────────────────────
//
// A real corpus punches holes in `iv21` wherever the 21d strip was
// unavailable — 14.2% of all (symbol, session) pairs on the shipped 25-name
// v2 panel, essentially all `var21_out_of_range`, i.e. sessions with a
// present surface and a valid spot. v1/v2/v3 DROP those rows while keeping
// their spots in every window, so the emitted file is a strict subset of the
// axis its own feature columns were computed on. This fixture reproduces
// that: same series, strip punched out on a scattered index set.
[[nodiscard]] VrpSeries make_vrp_series_with_strip_holes(int n) {
  VrpSeries s = make_vrp_series(n);
  // Bounded by n. `i % 7 == 3` is a deterministic scatter, not a run: it puts
  // holes INSIDE every 21-session window without ever making two adjacent, so
  // a consumer that merely skips them still lands on plausible-looking dates.
  for (std::size_t i = 0; i < s.iv21.size(); ++i) {
    if (i % 7 == 3) {
      s.iv21[i] = std::numeric_limits<double>::quiet_NaN();
    }
  }
  return s;
}

TEST(VrpPanelV4, EmitsEveryBarAxisSessionWhereV2DropsTheStriplessOnes) {
  const VrpSeries s = make_vrp_series_with_strip_holes(60);
  std::size_t holes = 0;
  for (const double v : s.iv21) {
    holes += std::isnan(v) ? 1U : 0U;
  }
  ASSERT_GT(holes, 0u) << "fixture must actually punch holes";

  VrpPanelCounters c2;
  const Result<std::vector<VrpPanelRow>> v2 = build_vrp_rows(s, c2, VrpPanelSchema::V2);
  ASSERT_TRUE(v2.has_value()) << v2.error().to_string();
  VrpPanelCounters c4;
  const Result<std::vector<VrpPanelRow>> v4 = build_vrp_rows(s, c4, VrpPanelSchema::V4);
  ASSERT_TRUE(v4.has_value()) << v4.error().to_string();

  EXPECT_EQ(v2->size(), 60u - holes);
  EXPECT_EQ(v4->size(), 60u);
  EXPECT_EQ(c4.n_rows_written, 60u);
}

TEST(VrpPanelV4, BarIndexIsTheBarAxisPositionAndIsGapFree) {
  const VrpSeries s = make_vrp_series_with_strip_holes(60);
  VrpPanelCounters c;
  const Result<std::vector<VrpPanelRow>> rows_r = build_vrp_rows(s, c, VrpPanelSchema::V4);
  ASSERT_TRUE(rows_r.has_value()) << rows_r.error().to_string();
  const std::vector<VrpPanelRow> &rows = *rows_r;
  ASSERT_EQ(rows.size(), 60u);
  // Bounded by row count. The whole point of the column: adjacency is
  // CHECKABLE, not inferred from dates and a trading calendar.
  for (std::size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(rows[i].bar_index, static_cast<std::int64_t>(i));
    EXPECT_EQ(rows[i].date, s.dates[i]);
  }
}

// v4 must be a pure SUPERSET: every row v2 emits, v4 emits byte-identically.
// Otherwise "additive schema" is a claim rather than a property.
TEST(VrpPanelV4, RowsV2AlsoEmitsAreBitIdenticalUnderV4) {
  const VrpSeries s = make_vrp_series_with_strip_holes(60);
  VrpPanelCounters c2;
  const Result<std::vector<VrpPanelRow>> v2 = build_vrp_rows(s, c2, VrpPanelSchema::V2);
  ASSERT_TRUE(v2.has_value()) << v2.error().to_string();
  VrpPanelCounters c4;
  const Result<std::vector<VrpPanelRow>> v4 = build_vrp_rows(s, c4, VrpPanelSchema::V4);
  ASSERT_TRUE(v4.has_value()) << v4.error().to_string();

  std::size_t matched = 0;
  // Bounded by v2 row count; v4 is date-sorted so a linear scan suffices.
  std::size_t j = 0;
  for (const VrpPanelRow &a : *v2) {
    while (j < v4->size() && (*v4)[j].date != a.date) {
      ++j;
    }
    ASSERT_LT(j, v4->size()) << "v4 dropped a row v2 emitted: " << a.date;
    const VrpPanelRow &b = (*v4)[j];
    EXPECT_EQ(a.entry_ts_ns, b.entry_ts_ns);
    EXPECT_TRUE(same_bits(a.spot, b.spot)) << a.date;
    EXPECT_TRUE(same_bits(a.iv_fair_21d, b.iv_fair_21d)) << a.date;
    EXPECT_TRUE(same_bits(a.iv_fair_63d, b.iv_fair_63d)) << a.date;
    EXPECT_TRUE(same_bits(a.rv_fwd_21d, b.rv_fwd_21d)) << a.date;
    EXPECT_TRUE(same_bits(a.label, b.label)) << a.date;
    EXPECT_TRUE(same_bits(a.f0_log_rv1, b.f0_log_rv1)) << a.date;
    EXPECT_TRUE(same_bits(a.f1_log_rv5, b.f1_log_rv5)) << a.date;
    EXPECT_TRUE(same_bits(a.f2_log_rv21, b.f2_log_rv21)) << a.date;
    EXPECT_TRUE(same_bits(a.f3_iv_level, b.f3_iv_level)) << a.date;
    EXPECT_TRUE(same_bits(a.f4_term_slope, b.f4_term_slope)) << a.date;
    EXPECT_TRUE(same_bits(a.f5_hv_iv_gap, b.f5_hv_iv_gap)) << a.date;
    EXPECT_TRUE(same_bits(a.f6_vrp_lag, b.f6_vrp_lag)) << a.date;
    EXPECT_TRUE(same_bits(a.f7_ret_21d, b.f7_ret_21d)) << a.date;
    EXPECT_TRUE(same_bits(a.f8_jump_recent, b.f8_jump_recent)) << a.date;
    EXPECT_TRUE(same_bits(a.f9_vov_63d, b.f9_vov_63d)) << a.date;
    EXPECT_TRUE(same_bits(a.iv_atmf_21d, b.iv_atmf_21d)) << a.date;
    ++matched;
  }
  EXPECT_EQ(matched, v2->size());
}

// The recovered rows must be honestly labelled: the implied leg and
// everything downstream of it is NaN, the spot-derived block is FULLY VALID.
// That block is the coverage v4 exists to recover — if it were NaN too the
// schema would buy nothing.
TEST(VrpPanelV4, StriplessRowsCarryNaNImpliedLegAndAValidSpotBlock) {
  // 120, not 60: f9's window is 63 sessions and the forward label eats 21
  // more, so a 60-session fixture leaves NO row that is past warmup AND has a
  // label — the trim would be vacuously empty and the test would pass on
  // nothing. The ASSERT_GT at the bottom is what caught that.
  const VrpSeries s = make_vrp_series_with_strip_holes(120);
  VrpPanelCounters c;
  const Result<std::vector<VrpPanelRow>> rows_r = build_vrp_rows(s, c, VrpPanelSchema::V4);
  ASSERT_TRUE(rows_r.has_value()) << rows_r.error().to_string();
  const std::vector<VrpPanelRow> &rows = *rows_r;

  std::size_t checked = 0;
  // Bounded by row count. Warmup/tail rows are excluded so "valid" means the
  // feature had its inputs, not that it happened to be defined.
  for (std::size_t i = 63; i + kVrpHorizonSessions < rows.size(); ++i) {
    if (!std::isnan(s.iv21[i])) {
      continue;
    }
    const VrpPanelRow &r = rows[i];
    EXPECT_TRUE(std::isnan(r.iv_fair_21d)) << i;
    EXPECT_TRUE(std::isnan(r.label)) << i;
    EXPECT_TRUE(std::isnan(r.f3_iv_level)) << i;
    EXPECT_TRUE(std::isnan(r.f4_term_slope)) << i;
    EXPECT_TRUE(std::isnan(r.f5_hv_iv_gap)) << i;
    EXPECT_TRUE(std::isnan(r.f6_vrp_lag)) << i;
    // ...and the spot side is intact.
    EXPECT_TRUE(std::isfinite(r.spot)) << i;
    EXPECT_TRUE(std::isfinite(r.rv_fwd_21d)) << i;
    EXPECT_TRUE(std::isfinite(r.f0_log_rv1)) << i;
    EXPECT_TRUE(std::isfinite(r.f1_log_rv5)) << i;
    EXPECT_TRUE(std::isfinite(r.f2_log_rv21)) << i;
    EXPECT_TRUE(std::isfinite(r.f7_ret_21d)) << i;
    EXPECT_TRUE(std::isfinite(r.f8_jump_recent)) << i;
    ++checked;
  }
  ASSERT_GT(checked, 0u) << "no strip-hole row survived the warmup/tail trim";
}

// THE DEFECT ITSELF, as a test. A downstream consumer reads the emitted file
// and recomputes a trailing 21-session window from the emitted `spot` column
// of the 22 preceding EMITTED rows. Under v4 that reproduces the panel's own
// f2_log_rv21 exactly, because the emitted axis IS the bar axis. Under v2 it
// does not — the window silently spans more sessions than it counts. The v2
// half is the anti-vacuity control: without it this test would pass on a
// schema that fixed nothing.
TEST(VrpPanelV4, TrailingWindowOverEmittedRowsReproducesTheColumnOnlyUnderV4) {
  const VrpSeries s = make_vrp_series_with_strip_holes(60);

  // Recompute f2 from a row batch's OWN spot column, treating consecutive
  // emitted rows as consecutive sessions — exactly what a consumer that never
  // saw the bar axis must assume. Returns the max |recomputed - emitted| over
  // every row with 21 predecessors, and the count of rows compared.
  const auto max_abs_gap = [](const std::vector<VrpPanelRow> &rows) {
    double worst = 0.0;
    // Bounded by row count.
    for (std::size_t i = 21; i < rows.size(); ++i) {
      double sum = 0.0;
      for (std::size_t j = i - 20; j <= i; ++j) {
        const double r = std::log(rows[j].spot / rows[j - 1].spot);
        sum += r * r;
      }
      const double got = std::log(sum / 21.0 * 252.0);
      worst = std::max(worst, std::abs(got - rows[i].f2_log_rv21));
    }
    return worst;
  };

  VrpPanelCounters c4;
  const Result<std::vector<VrpPanelRow>> v4 = build_vrp_rows(s, c4, VrpPanelSchema::V4);
  ASSERT_TRUE(v4.has_value()) << v4.error().to_string();
  EXPECT_LT(max_abs_gap(*v4), 1e-12) << "v4's emitted axis is not the bar axis";

  VrpPanelCounters c2;
  const Result<std::vector<VrpPanelRow>> v2 = build_vrp_rows(s, c2, VrpPanelSchema::V2);
  ASSERT_TRUE(v2.has_value()) << v2.error().to_string();
  EXPECT_GT(max_abs_gap(*v2), 1e-6)
      << "v2 recomputation agreed — the fixture stopped reproducing the defect";
}

TEST(VrpPanelV4, SchemaTableIsAPrefixExtensionOfV3PlusBarIndex) {
  ASSERT_EQ(schema_column_count(VrpPanelSchema::V4), kVrpPanelColumnCountV3 + 1);
  // Bounded by v3's column count.
  for (std::size_t i = 0; i < kVrpPanelColumnCountV3; ++i) {
    EXPECT_EQ(schema_column(VrpPanelSchema::V4, i), schema_column(VrpPanelSchema::V3, i)) << i;
  }
  EXPECT_EQ(schema_column(VrpPanelSchema::V4, kVrpPanelColumnCountV3), "bar_index");
  EXPECT_EQ(schema_name(VrpPanelSchema::V4), "vrp_panel_v4");
}

// ── The meta block must add up ───────────────────────────────────────────
//
// Reproduces the defect that motivated the guard: a real shipped panel carried
// n_symbols=25 / n_rows=5848 beside n_symbol_sessions=24888 (= 244 x 102) and
// n_var21_out_of_range=3544. Those numbers cannot describe one run, and every
// reader of that header -- including this project -- believed them.
TEST(VrpPanelCounters, RejectsTheShippedPanelHeaderThatDescribedADifferentRun) {
  VrpPanelCounters c;
  c.n_sessions = 244;
  c.n_symbol_sessions = 24888; // 244 * 102, but the file claimed 25 symbols
  c.n_no_surface = 683;
  c.n_var21_out_of_range = 3544;
  c.n_rows_written = 5848;
  const Status st = check_vrp_counters(c, VrpPanelSchema::V2, /*n_symbols=*/25);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::Internal);
  // 24888 - 683 - 3544 = 20661, nowhere near the 5848 rows the file held.
  EXPECT_NE(st.error().to_string().find("20661"), std::string::npos)
      << st.error().to_string();
}

TEST(VrpPanelCounters, AcceptsACoherentRunUnderEverySchema) {
  // The real 25-name run: 6100 = 244 * 25, and 6100 - 111 - 141 = 5848.
  VrpPanelCounters c;
  c.n_sessions = 244;
  c.n_symbol_sessions = 6100;
  c.n_no_surface = 111;
  c.n_var21_out_of_range = 141;
  c.n_rows_written = 5848;
  EXPECT_TRUE(check_vrp_counters(c, VrpPanelSchema::V1, 25).has_value());
  EXPECT_TRUE(check_vrp_counters(c, VrpPanelSchema::V2, 25).has_value());
  EXPECT_TRUE(check_vrp_counters(c, VrpPanelSchema::V3, 25).has_value());
  // v4 keeps the strip-less rows, so the SAME drops imply a different row
  // count -- and the real 616-name v4 run closes on exactly that identity:
  // 153384 - 13606 = 139778.
  EXPECT_FALSE(check_vrp_counters(c, VrpPanelSchema::V4, 25).has_value());
  VrpPanelCounters v4 = c;
  v4.n_rows_written = 6100 - 111; // v4 emits the var21 rows too
  EXPECT_TRUE(check_vrp_counters(v4, VrpPanelSchema::V4, 25).has_value());
}

TEST(VrpPanelCounters, CatchesASymbolCountThatDoesNotDivideTheAttempts) {
  VrpPanelCounters c;
  c.n_sessions = 244;
  c.n_symbol_sessions = 6100;
  c.n_no_surface = 111;
  c.n_var21_out_of_range = 141;
  c.n_rows_written = 5848;
  // Rows close, but 244 * 26 != 6100 -- the second identity is what pins the
  // header's `n_symbols` to the run that produced it.
  const Status st = check_vrp_counters(c, VrpPanelSchema::V2, /*n_symbols=*/26);
  ASSERT_FALSE(st.has_value());
  EXPECT_NE(st.error().to_string().find("n_symbol_sessions"), std::string::npos);
}

// A guard that only ever passes is not a guard. Every run this suite builds
// goes through it, so this test asserts the SHAPE the real emitter produces
// rather than a fixture -- it fails if the row policy and the counters ever
// stop agreeing.
TEST(VrpPanelCounters, TheRealEmitterClosesItsOwnAccounting) {
  const VrpSeries s = make_vrp_series_with_strip_holes(120);
  std::size_t holes = 0;
  for (const double v : s.iv21) {
    holes += std::isnan(v) ? 1U : 0U;
  }
  for (const VrpPanelSchema schema : {VrpPanelSchema::V2, VrpPanelSchema::V4}) {
    VrpPanelCounters c;
    // One symbol, one session per bar, nothing absent: the loader's own
    // bookkeeping for a series with no missing surfaces.
    c.n_sessions = s.spot.size();
    c.n_symbol_sessions = s.spot.size();
    c.n_var21_out_of_range = holes;
    const auto rows = build_vrp_rows(s, c, schema);
    ASSERT_TRUE(rows.has_value()) << rows.error().to_string();
    EXPECT_TRUE(check_vrp_counters(c, schema, /*n_symbols=*/1).has_value())
        << "schema " << schema_name(schema) << ": "
        << check_vrp_counters(c, schema, 1).error().to_string();
  }
}

// ── Quarantine: the unit of the defect is a STEP ─────────────────────────
//
// An unadjusted corporate action puts one impossible return in the middle of
// the spot series. The rv_fwd gate sees only part of the damage — the 21 rows
// whose FORWARD window spans it — while the 63 rows whose TRAILING windows
// span it carry a perfectly plausible rv_fwd and were never flagged at all.
// Quarantine works on the step, so it reaches both.
[[nodiscard]] VrpSeries make_vrp_series_with_split(int n, std::size_t ex_bar, double factor) {
  VrpSeries s = make_vrp_series(n);
  // Bounded by n. Everything from `ex_bar` on is divided, exactly as an
  // unadjusted vendor series looks after a forward split.
  for (std::size_t i = ex_bar; i < s.spot.size(); ++i) {
    s.spot[i] /= factor;
  }
  return s;
}

TEST(VrpPanelQuarantine, NaNsTheForwardLegOfEveryRowWhoseWindowSpansTheSplitStep) {
  constexpr std::size_t kEx = 120;
  const VrpSeries s = make_vrp_series_with_split(240, kEx, 10.0);
  VrpPanelCounters c;
  const Result<std::vector<VrpPanelRow>> rows_r =
      build_vrp_rows(s, c, VrpPanelSchema::V4, VrpImplausiblePolicy::Quarantine);
  ASSERT_TRUE(rows_r.has_value()) << rows_r.error().to_string();
  const std::vector<VrpPanelRow> &rows = *rows_r;
  EXPECT_EQ(c.n_implausible_steps, 1u);

  // The forward window of row i is bars i+1..i+21, whose RETURNS are steps
  // i+2..i+21. So step kEx is inside exactly rows kEx-21 .. kEx-2.
  for (std::size_t i = kEx - 21; i <= kEx - 2; ++i) {
    EXPECT_TRUE(std::isnan(rows[i].rv_fwd_21d)) << "row " << i << " kept a fictional label";
    EXPECT_TRUE(std::isnan(rows[i].label)) << i;
  }
  // Immediately outside that band the forward window is clean and must SURVIVE
  // — a quarantine that swallowed the neighbourhood would pass the test above
  // while destroying good data.
  EXPECT_FALSE(std::isnan(rows[kEx - 22].rv_fwd_21d));
  EXPECT_FALSE(std::isnan(rows[kEx - 1].rv_fwd_21d));
}

TEST(VrpPanelQuarantine, NaNsTrailingFeaturesTheRvGateNeverSees) {
  constexpr std::size_t kEx = 120;
  const VrpSeries s = make_vrp_series_with_split(240, kEx, 10.0);
  VrpPanelCounters c;
  const Result<std::vector<VrpPanelRow>> rows_r =
      build_vrp_rows(s, c, VrpPanelSchema::V4, VrpImplausiblePolicy::Quarantine);
  ASSERT_TRUE(rows_r.has_value()) << rows_r.error().to_string();
  const std::vector<VrpPanelRow> &rows = *rows_r;

  // Row kEx+10 sits ten sessions PAST the split: its rv_fwd is clean and the
  // gate would never flag it, but its trailing 21 window still spans the step.
  const VrpPanelRow &after = rows[kEx + 10];
  EXPECT_FALSE(std::isnan(after.rv_fwd_21d)) << "the forward leg here is genuinely clean";
  EXPECT_TRUE(std::isnan(after.f2_log_rv21));
  EXPECT_TRUE(std::isnan(after.f5_hv_iv_gap));
  EXPECT_TRUE(std::isnan(after.f6_vrp_lag));
  EXPECT_TRUE(std::isnan(after.f7_ret_21d));
  EXPECT_TRUE(std::isnan(after.f8_jump_recent));
  // Implied vol is scale-invariant: a share-count change does not move it, so
  // masking these would be superstition, not caution.
  EXPECT_FALSE(std::isnan(after.f3_iv_level));
  EXPECT_FALSE(std::isnan(after.f4_term_slope));

  // 63 sessions past the split every trailing window has cleared it.
  const VrpPanelRow &clear = rows[kEx + 63];
  EXPECT_FALSE(std::isnan(clear.f2_log_rv21));
  EXPECT_FALSE(std::isnan(clear.f8_jump_recent));
}

// The anti-vacuity control. Quarantine must be INERT on data with no
// implausible step: same rows, bit for bit, as the default policy. Without
// this a quarantine that NaN'd everything would pass both tests above.
TEST(VrpPanelQuarantine, IsBitIdenticalToFailOnASeriesWithNoImplausibleStep) {
  const VrpSeries s = make_vrp_series(240);
  VrpPanelCounters cf;
  const Result<std::vector<VrpPanelRow>> f =
      build_vrp_rows(s, cf, VrpPanelSchema::V4, VrpImplausiblePolicy::Fail);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  VrpPanelCounters cq;
  const Result<std::vector<VrpPanelRow>> q =
      build_vrp_rows(s, cq, VrpPanelSchema::V4, VrpImplausiblePolicy::Quarantine);
  ASSERT_TRUE(q.has_value()) << q.error().to_string();

  ASSERT_EQ(f->size(), q->size());
  EXPECT_EQ(cq.n_implausible_steps, 0u);
  EXPECT_EQ(cq.n_quarantined_forward, 0u);
  EXPECT_EQ(cq.n_quarantined_trailing, 0u);
  // Bounded by row count.
  for (std::size_t i = 0; i < f->size(); ++i) {
    const VrpPanelRow &a = (*f)[i];
    const VrpPanelRow &b = (*q)[i];
    EXPECT_TRUE(same_bits(a.rv_fwd_21d, b.rv_fwd_21d)) << i;
    EXPECT_TRUE(same_bits(a.label, b.label)) << i;
    EXPECT_TRUE(same_bits(a.f0_log_rv1, b.f0_log_rv1)) << i;
    EXPECT_TRUE(same_bits(a.f1_log_rv5, b.f1_log_rv5)) << i;
    EXPECT_TRUE(same_bits(a.f2_log_rv21, b.f2_log_rv21)) << i;
    EXPECT_TRUE(same_bits(a.f5_hv_iv_gap, b.f5_hv_iv_gap)) << i;
    EXPECT_TRUE(same_bits(a.f6_vrp_lag, b.f6_vrp_lag)) << i;
    EXPECT_TRUE(same_bits(a.f7_ret_21d, b.f7_ret_21d)) << i;
    EXPECT_TRUE(same_bits(a.f8_jump_recent, b.f8_jump_recent)) << i;
  }
}

// Detection is not the policy. The gate's own counter must be unmoved by
// quarantine, so the meta header reports the same honest number either way and
// a quarantined run cannot be mistaken for a clean corpus.
TEST(VrpPanelQuarantine, DetectionCountIsIdenticalUnderBothPolicies) {
  const VrpSeries s = make_vrp_series_with_split(240, 120, 10.0);
  VrpPanelCounters cf;
  ASSERT_TRUE(build_vrp_rows(s, cf, VrpPanelSchema::V4, VrpImplausiblePolicy::Fail).has_value());
  VrpPanelCounters cq;
  ASSERT_TRUE(
      build_vrp_rows(s, cq, VrpPanelSchema::V4, VrpImplausiblePolicy::Quarantine).has_value());
  EXPECT_EQ(cf.n_rv_fwd_implausible, cq.n_rv_fwd_implausible);
  EXPECT_GT(cf.n_rv_fwd_implausible, 0u);
}

// The step threshold is DERIVED from the rv gate, not chosen independently.
TEST(VrpPanelQuarantine, StepThresholdIsDerivedFromTheRvPlausibilityGate) {
  const double derived = std::sqrt(kVrpMaxPlausibleRvFwd * kVrpMaxPlausibleRvFwd *
                                   static_cast<double>(kVrpHorizonSessions - 1) / 252.0);
  EXPECT_NEAR(kVrpImplausibleStepReturn, derived, 1e-15);
}

// Done-criterion (3): the forward-RV window is EXACTLY sessions t+1..t+21 —
// a spike planted at t and one planted at t+22 each leave the label at t
// byte-identical, while a row whose window genuinely contains the spiked
// session DOES move (guards against a vacuous pass).
TEST(VrpPanel, ForwardWindowIsExactlySessionsTPlus1ToTPlus21) {
  const int n = 30;
  const std::size_t t = 5;
  const VrpSeries base = make_vrp_series(n);
  VrpPanelCounters c0;
  const Result<std::vector<VrpPanelRow>> rows0_r = build_vrp_rows(base, c0, VrpPanelSchema::V2);
  ASSERT_TRUE(rows0_r.has_value()) << rows0_r.error().to_string();
  const std::vector<VrpPanelRow> &rows0 = *rows0_r;

  VrpSeries spike_at_t = base;
  spike_at_t.spot[t] *= 1.25;
  VrpPanelCounters ca;
  const Result<std::vector<VrpPanelRow>> rows_a_r = build_vrp_rows(spike_at_t, ca, VrpPanelSchema::V2);
  ASSERT_TRUE(rows_a_r.has_value()) << rows_a_r.error().to_string();
  const std::vector<VrpPanelRow> &rows_a = *rows_a_r;
  EXPECT_TRUE(same_bits(rows_a[t].rv_fwd_21d, rows0[t].rv_fwd_21d))
      << "session t's own close must not enter the forward window";
  EXPECT_TRUE(same_bits(rows_a[t].label, rows0[t].label));
  // Sanity: row t-1's window (t..t+20) contains the spiked session.
  EXPECT_FALSE(same_bits(rows_a[t - 1].rv_fwd_21d, rows0[t - 1].rv_fwd_21d));

  VrpSeries spike_past_window = base;
  spike_past_window.spot[t + 22] *= 1.25;
  VrpPanelCounters cb;
  const Result<std::vector<VrpPanelRow>> rows_b_r = build_vrp_rows(spike_past_window, cb, VrpPanelSchema::V2);
  ASSERT_TRUE(rows_b_r.has_value()) << rows_b_r.error().to_string();
  const std::vector<VrpPanelRow> &rows_b = *rows_b_r;
  EXPECT_TRUE(same_bits(rows_b[t].rv_fwd_21d, rows0[t].rv_fwd_21d))
      << "session t+22 lies past the forward window";
  EXPECT_TRUE(same_bits(rows_b[t].label, rows0[t].label));
  // Sanity: row t+1's window (t+2..t+22) contains the spiked session.
  EXPECT_FALSE(same_bits(rows_b[t + 1].rv_fwd_21d, rows0[t + 1].rv_fwd_21d));
}

// Done-criterion (4): no-lookahead tripwire — perturbing EVERY session > t
// (spot AND both iv marks) leaves every feature column at rows <= t
// byte-identical; only the forward-looking rv_fwd/label may move.
TEST(VrpPanel, PerturbingFutureSessionsLeavesFeaturesByteIdentical) {
  const int n = 100;
  const std::size_t t = 70; // >= 63 so f8/f9 are real values, not warmup NaN
  const VrpSeries base = make_vrp_series(n);
  VrpPanelCounters c0;
  const Result<std::vector<VrpPanelRow>> rows0_r = build_vrp_rows(base, c0, VrpPanelSchema::V2);
  ASSERT_TRUE(rows0_r.has_value()) << rows0_r.error().to_string();
  const std::vector<VrpPanelRow> &rows0 = *rows0_r;
  ASSERT_TRUE(std::isfinite(rows0[t].f9_vov_63d)) << "t chosen past the f9 warmup";
  ASSERT_TRUE(std::isfinite(rows0[t].f8_jump_recent));

  VrpSeries fut = base;
  for (std::size_t s2 = t + 1; s2 < static_cast<std::size_t>(n); ++s2) {
    fut.spot[s2] *= 1.0 + 0.002 * static_cast<double>(s2 - t);
    fut.iv21[s2] += 0.004;
    fut.iv63[s2] += 0.006;
  }
  VrpPanelCounters c1;
  const Result<std::vector<VrpPanelRow>> rows1_r = build_vrp_rows(fut, c1, VrpPanelSchema::V2);
  ASSERT_TRUE(rows1_r.has_value()) << rows1_r.error().to_string();
  const std::vector<VrpPanelRow> &rows1 = *rows1_r;
  ASSERT_EQ(rows1.size(), rows0.size());

  for (std::size_t i = 0; i <= t; ++i) {
    EXPECT_EQ(rows1[i].date, rows0[i].date);
    EXPECT_EQ(rows1[i].entry_ts_ns, rows0[i].entry_ts_ns);
    EXPECT_TRUE(same_bits(rows1[i].spot, rows0[i].spot)) << i;
    EXPECT_TRUE(same_bits(rows1[i].iv_fair_21d, rows0[i].iv_fair_21d)) << i;
    EXPECT_TRUE(same_bits(rows1[i].iv_fair_63d, rows0[i].iv_fair_63d)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f0_log_rv1, rows0[i].f0_log_rv1)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f1_log_rv5, rows0[i].f1_log_rv5)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f2_log_rv21, rows0[i].f2_log_rv21)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f3_iv_level, rows0[i].f3_iv_level)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f4_term_slope, rows0[i].f4_term_slope)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f5_hv_iv_gap, rows0[i].f5_hv_iv_gap)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f6_vrp_lag, rows0[i].f6_vrp_lag)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f7_ret_21d, rows0[i].f7_ret_21d)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f8_jump_recent, rows0[i].f8_jump_recent)) << i;
    EXPECT_TRUE(same_bits(rows1[i].f9_vov_63d, rows0[i].f9_vov_63d)) << i;
  }
  // Sanity: the label at t DID move (its window is entirely in the
  // perturbed region), so the comparison above is not vacuous.
  EXPECT_FALSE(same_bits(rows1[t].label, rows0[t].label));
}

// Done-criterion (5): rows within 21 sessions of the tail emit NaN
// rv_fwd/label, are KEPT (predict-time rows), and are counted.
TEST(VrpPanel, TailRowsKeepNaNLabelAndAreCounted) {
  const int n = 30;
  const VrpSeries s = make_vrp_series(n);
  VrpPanelCounters c;
  const Result<std::vector<VrpPanelRow>> rows_r = build_vrp_rows(s, c, VrpPanelSchema::V2);
  ASSERT_TRUE(rows_r.has_value()) << rows_r.error().to_string();
  const std::vector<VrpPanelRow> &rows = *rows_r;
  ASSERT_EQ(rows.size(), 30u);
  EXPECT_EQ(c.n_rows_written, 30u);
  EXPECT_EQ(c.n_rows_tail_nan_label, 21u); // sessions 9..29 lack 21 future bars
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i + 21 < rows.size()) {
      EXPECT_TRUE(std::isfinite(rows[i].label)) << i;
      EXPECT_TRUE(std::isfinite(rows[i].rv_fwd_21d)) << i;
    } else {
      EXPECT_TRUE(std::isnan(rows[i].label)) << i;
      EXPECT_TRUE(std::isnan(rows[i].rv_fwd_21d)) << i;
    }
  }
}

// Done-criterion (1): the frozen vrp_panel_v1 file shape — the two frozen
// comment lines first, the 18 column names in exactly the frozen order, 18
// fields on every data row — over a real SurfaceDb round trip. v1 is now
// opt-in (v2 is the default), so this is also the flag's regression anchor.
TEST(VrpPanel, SchemaHeaderAndColumnOrderFrozen) {
  static_assert(kVrpPanelColumnCount == 18, "vrp_panel_v1 is frozen at 18 columns");

  const std::string root = (fs::temp_directory_path() / "atx-vrp-panel-schema-db").string();
  write_vrp_corpus(root, 0, 6, /*narrow_pillars=*/false);
  const std::string out = (fs::temp_directory_path() / "atx-vrp-panel-schema.tsv").string();

  VrpPanelConfig cfg;
  cfg.db_roots = {root};
  cfg.symbols = {"SPY"};
  cfg.out = out;
  cfg.schema = VrpPanelSchema::V1;
  const Result<VrpPanelCounters> rc = run_vrp_panel(cfg);
  ASSERT_TRUE(rc.has_value()) << rc.error().to_string();
  EXPECT_EQ(rc->n_sessions, 6u);
  EXPECT_EQ(rc->n_rows_written, 6u);
  EXPECT_EQ(rc->n_rows_tail_nan_label, 6u); // only 6 sessions: all predict-time
  EXPECT_EQ(rc->n_var21_out_of_range, 0u);

  const std::vector<char> bytes = read_whole_file(out);
  ASSERT_FALSE(bytes.empty());
  const std::string content(bytes.begin(), bytes.end());
  const std::string frozen_prefix = "# schema=vrp_panel_v1\n# horizon_days=21\n";
  ASSERT_GE(content.size(), frozen_prefix.size());
  EXPECT_EQ(content.compare(0, frozen_prefix.size(), frozen_prefix), 0)
      << "the two frozen comment lines must lead the file";

  std::string parsed_content;
  std::string_view header;
  std::vector<std::vector<std::string_view>> rows;
  parse_full_tsv(out, parsed_content, header, rows);
  std::string expected_header;
  for (std::size_t i = 0; i < kVrpPanelColumnCount; ++i) {
    if (i > 0) {
      expected_header += '\t';
    }
    expected_header += kVrpPanelColumnsV1[i];
  }
  EXPECT_EQ(header, expected_header) << "column names/order are frozen (vrp_panel_v1)";
  ASSERT_EQ(rows.size(), 6u);
  for (const auto &row : rows) {
    ASSERT_EQ(row.size(), kVrpPanelColumnCount);
    EXPECT_EQ(row[0], "SPY");
  }
  // NaN spelling is canonical: NaN-propagating arithmetic in the warmup
  // features carries a sign bit the UCRT would print as "-nan(ind)" — the
  // writer must emit exactly "nan" (row 0's f1_log_rv5 is such a warmup NaN).
  EXPECT_EQ(content.find("-nan"), std::string::npos) << "non-canonical NaN spelling leaked";
  EXPECT_EQ(rows[0][9], "nan"); // f1_log_rv5 warmup cell
}

// Done-criterion (6): a surface whose fitted pillars stop below 63/252
// yields OutOfRange at the slope tenor only — iv_fair_63d and f4_term_slope
// are the literal "nan" while the row itself is KEPT with a live 21d strike.
TEST(VrpPanel, OutOfRange63dYieldsNaNSlopeWithoutDroppingRow) {
  const std::string root = (fs::temp_directory_path() / "atx-vrp-panel-narrow-db").string();
  write_vrp_corpus(root, 0, 5, /*narrow_pillars=*/true);
  const std::string out = (fs::temp_directory_path() / "atx-vrp-panel-narrow.tsv").string();

  VrpPanelConfig cfg;
  cfg.db_roots = {root};
  cfg.symbols = {"SPY"};
  cfg.out = out;
  const Result<VrpPanelCounters> rc = run_vrp_panel(cfg);
  ASSERT_TRUE(rc.has_value()) << rc.error().to_string();
  EXPECT_EQ(rc->n_rows_written, 5u) << "63d OutOfRange must not drop rows";
  EXPECT_EQ(rc->n_63d_unavailable, 5u);
  EXPECT_EQ(rc->n_var21_out_of_range, 0u);

  std::string content;
  std::string_view header;
  std::vector<std::vector<std::string_view>> rows;
  parse_full_tsv(out, content, header, rows);
  ASSERT_EQ(rows.size(), 5u);
  for (const auto &row : rows) {
    ASSERT_EQ(row.size(), kVrpPanelColumnCountV2); // default schema is v2
    EXPECT_NE(row[kColIvFair21], "nan") << "21d strike must be live";
    EXPECT_EQ(row[kColIvFair63], "nan");
    EXPECT_EQ(row[kColTermSlope], "nan");
  }
}

// Done-criterion (7): stitching two roots produces a byte-identical panel to
// the same sessions written into one concatenated root.
TEST(VrpPanel, MultiRootStitchingMatchesSingleConcatenatedRoot) {
  const std::string root_a = (fs::temp_directory_path() / "atx-vrp-panel-stitch-a").string();
  const std::string root_b = (fs::temp_directory_path() / "atx-vrp-panel-stitch-b").string();
  const std::string root_c = (fs::temp_directory_path() / "atx-vrp-panel-stitch-c").string();
  write_vrp_corpus(root_a, 0, 6, /*narrow_pillars=*/false);
  write_vrp_corpus(root_b, 6, 6, /*narrow_pillars=*/false);
  write_vrp_corpus(root_c, 0, 12, /*narrow_pillars=*/false);

  const std::string out_ab = (fs::temp_directory_path() / "atx-vrp-panel-stitch-ab.tsv").string();
  const std::string out_c = (fs::temp_directory_path() / "atx-vrp-panel-stitch-c.tsv").string();

  VrpPanelConfig cfg;
  cfg.symbols = {"SPY"};
  cfg.db_roots = {root_a, root_b};
  cfg.out = out_ab;
  const Result<VrpPanelCounters> rc_ab = run_vrp_panel(cfg);
  ASSERT_TRUE(rc_ab.has_value()) << rc_ab.error().to_string();
  cfg.db_roots = {root_c};
  cfg.out = out_c;
  const Result<VrpPanelCounters> rc_c = run_vrp_panel(cfg);
  ASSERT_TRUE(rc_c.has_value()) << rc_c.error().to_string();

  EXPECT_EQ(rc_ab->n_sessions, 12u);
  EXPECT_EQ(rc_c->n_sessions, 12u);
  EXPECT_EQ(rc_ab->n_rows_written, rc_c->n_rows_written);
  const std::vector<char> bytes_ab = read_whole_file(out_ab);
  const std::vector<char> bytes_c = read_whole_file(out_c);
  ASSERT_FALSE(bytes_ab.empty());
  ASSERT_EQ(bytes_ab.size(), bytes_c.size());
  EXPECT_EQ(0, std::memcmp(bytes_ab.data(), bytes_c.data(), bytes_ab.size()))
      << "stitched multi-root panel must be byte-identical to the concatenated-root panel";
}

// A session date served by two roots is ambiguous — refused loudly, not
// silently double-counted.
TEST(VrpPanel, DuplicateSessionDateAcrossRootsIsRejected) {
  const std::string root_a = (fs::temp_directory_path() / "atx-vrp-panel-dup-a").string();
  const std::string root_b = (fs::temp_directory_path() / "atx-vrp-panel-dup-b").string();
  write_vrp_corpus(root_a, 0, 3, /*narrow_pillars=*/false);
  write_vrp_corpus(root_b, 0, 3, /*narrow_pillars=*/false);

  VrpPanelConfig cfg;
  cfg.symbols = {"SPY"};
  cfg.db_roots = {root_a, root_b};
  cfg.out = (fs::temp_directory_path() / "atx-vrp-panel-dup.tsv").string();
  const Result<VrpPanelCounters> rc = run_vrp_panel(cfg);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), ErrorCode::InvalidArgument);
}

// ════════════════════════════════════════════════════════════════════════
// VrpPanelV2 — round-4 contract: the ATM-forward implied leg, the split /
// corporate-action back-adjustment, and the permanent realized-vol
// plausibility gate. v1 must stay byte-frozen throughout.
// ════════════════════════════════════════════════════════════════════════

namespace {

// Write `text` to a temp file and hand back its path (split-reference
// fixtures — the loader contracts on bytes, so the tests supply bytes).
[[nodiscard]] std::string write_temp_text(const std::string &name, std::string_view text) {
  const std::string path = (fs::temp_directory_path() / name).string();
  std::ofstream os(path, std::ios::binary | std::ios::trunc);
  os.write(text.data(), static_cast<std::streamsize>(text.size()));
  return path;
}

// Split the meta block (leading `#` lines) off a panel file.
[[nodiscard]] std::vector<std::string> meta_lines(const std::string &path) {
  std::vector<std::string> out;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line) && !line.empty() && line[0] == '#') {
    out.push_back(line);
  }
  return out;
}

} // namespace

// The v2 contract is a strict PREFIX-EXTENSION of v1 in all three blocks:
// meta lines, column names, and per-row fields. Same corpus, no split
// reference — so the only difference the file may carry is the appended
// ATM-forward column and the four appended v2 counters.
TEST(VrpPanelV2, V2IsPrefixExtensionOfV1OnTheSameCorpus) {
  static_assert(kVrpPanelColumnCountV2 == kVrpPanelColumnCount + 1,
                "v2 adds exactly one column to the frozen v1 contract");
  const std::string root = (fs::temp_directory_path() / "atx-vrp-panel-v2-prefix-db").string();
  write_vrp_corpus(root, 0, 8, /*narrow_pillars=*/false);
  const std::string out_v1 = (fs::temp_directory_path() / "atx-vrp-panel-v2-prefix-v1.tsv").string();
  const std::string out_v2 = (fs::temp_directory_path() / "atx-vrp-panel-v2-prefix-v2.tsv").string();

  VrpPanelConfig cfg;
  cfg.db_roots = {root};
  cfg.symbols = {"SPY"};
  cfg.out = out_v1;
  cfg.schema = VrpPanelSchema::V1;
  const Result<VrpPanelCounters> rc1 = run_vrp_panel(cfg);
  ASSERT_TRUE(rc1.has_value()) << rc1.error().to_string();
  cfg.out = out_v2;
  cfg.schema = VrpPanelSchema::V2;
  const Result<VrpPanelCounters> rc2 = run_vrp_panel(cfg);
  ASSERT_TRUE(rc2.has_value()) << rc2.error().to_string();
  EXPECT_EQ(rc1->n_rows_written, rc2->n_rows_written) << "the row POLICY is unchanged by v2";

  const std::vector<std::string> meta1 = meta_lines(out_v1);
  const std::vector<std::string> meta2 = meta_lines(out_v2);
  ASSERT_EQ(meta1.size(), 13u);
  ASSERT_EQ(meta2.size(), 17u);
  EXPECT_EQ(meta1[0], "# schema=vrp_panel_v1");
  EXPECT_EQ(meta2[0], "# schema=vrp_panel_v2");
  // Every meta line after the schema tag is shared, in order.
  for (std::size_t i = 1; i < meta1.size(); ++i) {
    EXPECT_EQ(meta1[i], meta2[i]) << "v2 must not rewrite a v1 meta line (index " << i << ')';
  }
  EXPECT_EQ(meta2[13], "# n_atmf21_unavailable=0");
  EXPECT_EQ(meta2[14], "# n_split_events_applied=0");
  EXPECT_EQ(meta2[15], "# n_split_symbols_adjusted=0");
  EXPECT_EQ(meta2[16], "# n_rv_fwd_implausible=0");

  std::string c1;
  std::string c2;
  std::string_view h1;
  std::string_view h2;
  std::vector<std::vector<std::string_view>> r1;
  std::vector<std::vector<std::string_view>> r2;
  parse_full_tsv(out_v1, c1, h1, r1);
  parse_full_tsv(out_v2, c2, h2, r2);
  EXPECT_EQ(std::string(h2), std::string(h1) + "\tiv_atmf_21d");
  ASSERT_EQ(r1.size(), r2.size());
  for (std::size_t i = 0; i < r1.size(); ++i) {
    ASSERT_EQ(r1[i].size(), kVrpPanelColumnCount);
    ASSERT_EQ(r2[i].size(), kVrpPanelColumnCountV2);
    for (std::size_t j = 0; j < kVrpPanelColumnCount; ++j) {
      EXPECT_EQ(r1[i][j], r2[i][j]) << "row " << i << " column " << kVrpPanelColumnsV1[j];
    }
    // The tradeable leg is present, positive, and NOT the strip: on a skewed
    // smile K_var > sigma_ATMF^2 strictly, so the ATMF read must be smaller.
    const double atmf = std::stod(std::string(r2[i][kVrpPanelColumnCountV2 - 1]));
    const double strip = std::stod(std::string(r2[i][kColIvFair21]));
    EXPECT_GT(atmf, 0.0);
    EXPECT_LT(atmf, strip) << "ATM-forward vol must sit under the OTM-strip fair strike";
  }
}

// v1 is a frozen UNADJUSTED contract: it must be reproducible from the corpus
// alone, so the one input that would silently move a v1 cell is refused — at
// the CLI boundary and again in the runner.
TEST(VrpPanelV2, V1RefusesASplitReference) {
  const std::string splits =
      write_temp_text("atx-vrp-splits-refused.tsv", "symbol\tex_date\tprice_factor\nSPY\t2026-03-05\t0.1\n");

  std::vector<std::string> argv_storage = {"bev_label_factory", "--vrp-panel", "--db",    "rootA",
                                           "--out",             "p.tsv",       "--splits", splits,
                                           "--panel-schema",    "v1"};
  std::vector<char *> argv = make_argv(argv_storage);
  VrpPanelConfig bad;
  EXPECT_FALSE(parse_vrp_panel_args(static_cast<int>(argv.size()), argv.data(), bad));

  VrpPanelConfig cfg;
  cfg.db_roots = {"unused-root"};
  cfg.out = "unused.tsv";
  cfg.schema = VrpPanelSchema::V1;
  cfg.splits = splits;
  const Result<VrpPanelCounters> rc = run_vrp_panel(cfg);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), ErrorCode::InvalidArgument);
}

// CLI surface for the two v2 flags, including the schema-word validation.
TEST(VrpPanelV2, CliParsesSchemaAndSplitsFlags) {
  std::vector<std::string> ok_storage = {"bev_label_factory", "--vrp-panel",    "--db",  "rootA",
                                         "--out",             "p.tsv",          "--splits",
                                         "s.tsv",             "--panel-schema", "v2"};
  std::vector<char *> ok_argv = make_argv(ok_storage);
  VrpPanelConfig cfg;
  ASSERT_TRUE(parse_vrp_panel_args(static_cast<int>(ok_argv.size()), ok_argv.data(), cfg));
  EXPECT_EQ(cfg.schema, VrpPanelSchema::V2);
  EXPECT_EQ(cfg.splits, "s.tsv");

  // Default (no --panel-schema) is v2 — v1 is the opt-in legacy path.
  std::vector<std::string> def_storage = {"bev_label_factory", "--vrp-panel", "--db",
                                          "rootA",             "--out",       "p.tsv"};
  std::vector<char *> def_argv = make_argv(def_storage);
  VrpPanelConfig def_cfg;
  ASSERT_TRUE(parse_vrp_panel_args(static_cast<int>(def_argv.size()), def_argv.data(), def_cfg));
  EXPECT_EQ(def_cfg.schema, VrpPanelSchema::V2);
  EXPECT_TRUE(def_cfg.splits.empty());

  // v3 was this assertion's known-BAD value until the liquidity columns landed.
  // It is now a real schema, so the negative case moves to a value that is still
  // not one -- keeping a rejection test that actually tests rejection.
  std::vector<std::string> v3_storage = {"bev_label_factory", "--vrp-panel",    "--db", "rootA",
                                         "--out",             "p.tsv",          "--panel-schema",
                                         "v3"};
  std::vector<char *> v3_argv = make_argv(v3_storage);
  VrpPanelConfig v3_cfg;
  ASSERT_TRUE(parse_vrp_panel_args(static_cast<int>(v3_argv.size()), v3_argv.data(), v3_cfg));
  EXPECT_EQ(v3_cfg.schema, VrpPanelSchema::V3);
  EXPECT_TRUE(v3_cfg.liquidity.empty());

  std::vector<std::string> bad_storage = {"bev_label_factory", "--vrp-panel",    "--db", "rootA",
                                          "--out",             "p.tsv",          "--panel-schema",
                                          "v9"};
  std::vector<char *> bad_argv = make_argv(bad_storage);
  VrpPanelConfig bad_cfg;
  EXPECT_FALSE(parse_vrp_panel_args(static_cast<int>(bad_argv.size()), bad_argv.data(), bad_cfg));

  // --liquidity is v3-only, and the CLI must refuse it at parse time rather than
  // accept a flag it would then silently ignore.
  std::vector<std::string> liq_storage = {"bev_label_factory", "--vrp-panel", "--db",
                                          "rootA",             "--out",       "p.tsv",
                                          "--liquidity",       "liq.tsv"};
  std::vector<char *> liq_argv = make_argv(liq_storage);
  VrpPanelConfig liq_cfg;
  EXPECT_FALSE(parse_vrp_panel_args(static_cast<int>(liq_argv.size()), liq_argv.data(), liq_cfg));
}

// Reference-file grammar: comments and blank lines skipped, extra provenance
// columns ignored, output sorted by (symbol, ex_date).
TEST(VrpPanelV2, SplitReferenceLoaderParsesGrammarAndSorts) {
  const std::string path = write_temp_text(
      "atx-vrp-splits-ok.tsv",
      "# generated by vrp_split_factors.py\n"
      "symbol\tex_date\tprice_factor\tsource\traw_close\n"
      "NOW\t2025-12-18\t0.2\tequity_daily_bars\t153.38\n"
      "\n"
      "# NFLX 10-for-1\n"
      "NFLX\t2025-11-17\t0.1\tequity_daily_bars\t110.29\n"
      "BKNG\t2026-04-06\t0.04\tequity_daily_bars\t176.19\n");
  const Result<std::vector<VrpSplitFactor>> ev = load_vrp_split_factors(path);
  ASSERT_TRUE(ev.has_value()) << ev.error().to_string();
  ASSERT_EQ(ev->size(), 3u);
  EXPECT_EQ((*ev)[0].symbol, "BKNG");
  EXPECT_EQ((*ev)[1].symbol, "NFLX");
  EXPECT_EQ((*ev)[2].symbol, "NOW");
  EXPECT_EQ((*ev)[1].ex_date, "2025-11-17");
  EXPECT_DOUBLE_EQ((*ev)[0].price_factor, 0.04);
}

// Every malformed reference is refused at the boundary; nothing degrades to a
// silently-unadjusted series.
TEST(VrpPanelV2, SplitReferenceLoaderRejectsMalformedInput) {
  const auto load = [](const char *name, std::string_view text) {
    return load_vrp_split_factors(write_temp_text(name, text));
  };
  EXPECT_EQ(load("atx-vrp-splits-nohdr.tsv", "SPY\t2026-03-05\t0.1\n").error().code(),
            ErrorCode::ParseError);
  EXPECT_EQ(load("atx-vrp-splits-badhdr.tsv", "sym\tdate\tfactor\n").error().code(),
            ErrorCode::ParseError);
  EXPECT_EQ(load("atx-vrp-splits-short.tsv", "symbol\tex_date\tprice_factor\nSPY\t2026-03-05\n")
                .error()
                .code(),
            ErrorCode::ParseError);
  EXPECT_EQ(
      load("atx-vrp-splits-nan.tsv", "symbol\tex_date\tprice_factor\nSPY\t2026-03-05\tabc\n")
          .error()
          .code(),
      ErrorCode::ParseError);
  EXPECT_EQ(
      load("atx-vrp-splits-zero.tsv", "symbol\tex_date\tprice_factor\nSPY\t2026-03-05\t0\n")
          .error()
          .code(),
      ErrorCode::ParseError);
  EXPECT_EQ(load("atx-vrp-splits-neg.tsv", "symbol\tex_date\tprice_factor\nSPY\t2026-03-05\t-2\n")
                .error()
                .code(),
            ErrorCode::ParseError);
  EXPECT_EQ(load("atx-vrp-splits-dup.tsv",
                 "symbol\tex_date\tprice_factor\nSPY\t2026-03-05\t0.5\nSPY\t2026-03-05\t0.1\n")
                .error()
                .code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(load_vrp_split_factors("no-such-splits-file.tsv").error().code(), ErrorCode::IoError);
}

// The exactness property. A 2-for-1 factor is a power of two, so the whole
// adjusted series is `orig * 0.5` with NO rounding, and every panel quantity
// derived from a RATIO of two closes must come back BIT-IDENTICAL to the
// unsplit panel. Only the `spot` level itself may move.
TEST(VrpPanelV2, SplitAdjustmentRestoresEveryRatioQuantityBitExactly) {
  constexpr int kN = 40;
  constexpr std::size_t kEx = 20;
  const VrpSeries unsplit = make_vrp_series(kN);
  VrpSeries raw = unsplit;
  // Bounded by kN: the raw SurfaceDb spot mirror steps down at the ex-date.
  for (std::size_t i = kEx; i < static_cast<std::size_t>(kN); ++i) {
    raw.spot[i] *= 0.5;
  }
  VrpSeries adjusted = raw;
  const std::vector<VrpSplitFactor> events{{"SPY", unsplit.dates[kEx], 0.5}};
  EXPECT_EQ(apply_vrp_split_adjustment(adjusted, events), 1u);

  // Anchor invariant: the newest session is never rescaled.
  EXPECT_TRUE(same_bits(adjusted.spot.back(), raw.spot.back()));
  // Pre-ex sessions carry the factor; on/after sessions do not.
  for (std::size_t i = 0; i < static_cast<std::size_t>(kN); ++i) {
    EXPECT_TRUE(same_bits(adjusted.spot[i], unsplit.spot[i] * 0.5)) << i;
  }

  VrpPanelCounters c_unsplit;
  VrpPanelCounters c_adjusted;
  const Result<std::vector<VrpPanelRow>> a = build_vrp_rows(unsplit, c_unsplit, VrpPanelSchema::V2);
  const Result<std::vector<VrpPanelRow>> b = build_vrp_rows(adjusted, c_adjusted, VrpPanelSchema::V2);
  ASSERT_TRUE(a.has_value() && b.has_value());
  ASSERT_EQ(a->size(), b->size());
  for (std::size_t i = 0; i < a->size(); ++i) {
    const VrpPanelRow &x = (*a)[i];
    const VrpPanelRow &y = (*b)[i];
    EXPECT_TRUE(same_bits(y.spot, x.spot * 0.5)) << i;
    EXPECT_TRUE(same_bits(x.rv_fwd_21d, y.rv_fwd_21d)) << i;
    EXPECT_TRUE(same_bits(x.label, y.label)) << i;
    EXPECT_TRUE(same_bits(x.f0_log_rv1, y.f0_log_rv1)) << i;
    EXPECT_TRUE(same_bits(x.f1_log_rv5, y.f1_log_rv5)) << i;
    EXPECT_TRUE(same_bits(x.f2_log_rv21, y.f2_log_rv21)) << i;
    EXPECT_TRUE(same_bits(x.f5_hv_iv_gap, y.f5_hv_iv_gap)) << i;
    EXPECT_TRUE(same_bits(x.f6_vrp_lag, y.f6_vrp_lag)) << i;
    EXPECT_TRUE(same_bits(x.f7_ret_21d, y.f7_ret_21d)) << i;
    EXPECT_TRUE(same_bits(x.f8_jump_recent, y.f8_jump_recent)) << i;
  }
}

// The defect this whole feature exists for: a 10-for-1 step drives rv_fwd_21d
// past the plausibility gate on every window that straddles it; the reference
// factor removes it and the gate goes quiet.
TEST(VrpPanelV2, SplitAdjustmentClearsTheImplausibleRealizedVolCounter) {
  constexpr int kN = 40;
  constexpr std::size_t kEx = 20;
  const VrpSeries unsplit = make_vrp_series(kN);
  VrpSeries raw = unsplit;
  for (std::size_t i = kEx; i < static_cast<std::size_t>(kN); ++i) {
    raw.spot[i] *= 0.1;
  }
  VrpPanelCounters c_raw;
  const Result<std::vector<VrpPanelRow>> rows_raw = build_vrp_rows(raw, c_raw, VrpPanelSchema::V2);
  ASSERT_TRUE(rows_raw.has_value());
  // Row i's forward window carries the returns r_{i+2}..r_{i+21}, so the
  // corrupt return at bar kEx lands in every labeled row (i <= kN-22 == 18).
  EXPECT_EQ(c_raw.n_rv_fwd_implausible, 19u) << "every window straddling the step must trip";
  EXPECT_GT(rows_raw->at(18).rv_fwd_21d, kVrpMaxPlausibleRvFwd);

  VrpSeries adjusted = raw;
  const std::vector<VrpSplitFactor> events{{"SPY", unsplit.dates[kEx], 0.1}};
  ASSERT_EQ(apply_vrp_split_adjustment(adjusted, events), 1u);
  VrpPanelCounters c_adj;
  const Result<std::vector<VrpPanelRow>> rows_adj = build_vrp_rows(adjusted, c_adj, VrpPanelSchema::V2);
  ASSERT_TRUE(rows_adj.has_value());
  EXPECT_EQ(c_adj.n_rv_fwd_implausible, 0u);
  VrpPanelCounters c_ref;
  const Result<std::vector<VrpPanelRow>> rows_ref = build_vrp_rows(unsplit, c_ref, VrpPanelSchema::V2);
  ASSERT_TRUE(rows_ref.has_value());
  for (std::size_t i = 0; i < rows_ref->size(); ++i) {
    // The tail rows are NaN by design (no forward window) — assert the NaN
    // structure survives, then the value on the labeled rows.
    ASSERT_EQ(std::isnan(rows_adj->at(i).rv_fwd_21d), std::isnan(rows_ref->at(i).rv_fwd_21d)) << i;
    if (std::isnan(rows_ref->at(i).rv_fwd_21d)) {
      continue;
    }
    EXPECT_NEAR(rows_adj->at(i).rv_fwd_21d, rows_ref->at(i).rv_fwd_21d, 1e-12) << i;
    EXPECT_NEAR(rows_adj->at(i).label, rows_ref->at(i).label, 1e-15) << i;
  }
}

// Events the history cannot express are ignored rather than silently
// rescaling the whole series (a uniform rescale is a no-op on every return,
// but it would move the emitted `spot` column for no reason).
TEST(VrpPanelV2, SplitAdjustmentIgnoresEventsOutsideTheSessionHistory) {
  const VrpSeries base = make_vrp_series(10);
  const auto unchanged = [&base](const VrpSeries &s) {
    for (std::size_t i = 0; i < base.spot.size(); ++i) {
      if (!same_bits(s.spot[i], base.spot[i])) {
        return false;
      }
    }
    return true;
  };
  VrpSeries before = base;
  EXPECT_EQ(apply_vrp_split_adjustment(before, std::vector<VrpSplitFactor>{{"SPY", "a", 0.5}}), 0u);
  EXPECT_TRUE(unchanged(before));

  VrpSeries at_first = base;
  EXPECT_EQ(apply_vrp_split_adjustment(at_first,
                                       std::vector<VrpSplitFactor>{{"SPY", base.dates.front(), 0.5}}),
            0u);
  EXPECT_TRUE(unchanged(at_first)) << "no session precedes the first, so there is nothing to scale";

  VrpSeries after = base;
  EXPECT_EQ(apply_vrp_split_adjustment(after, std::vector<VrpSplitFactor>{{"SPY", "z", 0.5}}), 0u);
  EXPECT_TRUE(unchanged(after));

  // At the LAST session the event is real: everything before it scales.
  VrpSeries at_last = base;
  EXPECT_EQ(
      apply_vrp_split_adjustment(at_last, std::vector<VrpSplitFactor>{{"SPY", base.dates.back(), 0.5}}),
      1u);
  EXPECT_TRUE(same_bits(at_last.spot.back(), base.spot.back()));
  EXPECT_TRUE(same_bits(at_last.spot.front(), base.spot.front() * 0.5));
}

// Cumulative composition: two events compound on every session that precedes
// both, exactly once each.
TEST(VrpPanelV2, SplitAdjustmentComposesMultipleEventsCumulatively) {
  const VrpSeries base = make_vrp_series(12);
  VrpSeries s = base;
  const std::vector<VrpSplitFactor> events{{"SPY", base.dates[4], 0.5}, {"SPY", base.dates[8], 0.25}};
  ASSERT_EQ(apply_vrp_split_adjustment(s, events), 2u);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(same_bits(s.spot[i], base.spot[i] * 0.125)) << i; // 0.5 * 0.25
  }
  for (std::size_t i = 4; i < 8; ++i) {
    EXPECT_TRUE(same_bits(s.spot[i], base.spot[i] * 0.25)) << i;
  }
  for (std::size_t i = 8; i < 12; ++i) {
    EXPECT_TRUE(same_bits(s.spot[i], base.spot[i])) << i;
  }
}

// End-to-end: a v2 run over a corpus carrying an unadjusted 10-for-1 split
// FAILS loudly, and the identical run with the reference factor supplied
// succeeds with the gate counter at zero. v1, being the frozen unadjusted
// contract, still emits the corrupt panel — that is exactly why v1 is not the
// default any more.
TEST(VrpPanelV2, UnadjustedSplitFailsTheV2RunAndTheReferenceFactorClearsIt) {
  const std::string root = (fs::temp_directory_path() / "atx-vrp-panel-v2-split-db").string();
  write_vrp_corpus(root, 0, 26, /*narrow_pillars=*/false, /*split_day=*/13);

  VrpPanelConfig cfg;
  cfg.db_roots = {root};
  cfg.symbols = {"SPY"};
  cfg.out = (fs::temp_directory_path() / "atx-vrp-panel-v2-split.tsv").string();
  const Result<VrpPanelCounters> failed = run_vrp_panel(cfg);
  ASSERT_FALSE(failed.has_value()) << "an unadjusted split must not produce a v2 panel";
  EXPECT_EQ(failed.error().code(), ErrorCode::OutOfRange);
  EXPECT_NE(failed.error().to_string().find("--splits"), std::string::npos)
      << "the gate must name the remedy: " << failed.error().to_string();
  EXPECT_NE(failed.error().to_string().find("SPY"), std::string::npos)
      << "the gate must name the offending symbol: " << failed.error().to_string();

  // date_for_day(13) is the ex-date; the reference factor is the 10-for-1.
  cfg.splits = write_temp_text("atx-vrp-splits-corpus.tsv",
                               "symbol\tex_date\tprice_factor\nSPY\t" + date_for_day(13) + "\t0.1\n");
  const Result<VrpPanelCounters> ok = run_vrp_panel(cfg);
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();
  EXPECT_EQ(ok->n_rv_fwd_implausible, 0u);
  EXPECT_EQ(ok->n_split_events_applied, 1u);
  EXPECT_EQ(ok->n_split_symbols_adjusted, 1u);

  std::string content;
  std::string_view header;
  std::vector<std::vector<std::string_view>> rows;
  parse_full_tsv(cfg.out, content, header, rows);
  ASSERT_FALSE(rows.empty());
  EXPECT_NE(content.find("# n_split_events_applied=1"), std::string::npos);
  EXPECT_NE(content.find("# n_rv_fwd_implausible=0"), std::string::npos);
}
