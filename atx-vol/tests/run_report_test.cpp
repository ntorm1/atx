// atx-vol run_report emitters — machine-readable run-output gate tests.
//
// `run_report.hpp` pins an exact file shape (meta header + CSV rows) and exact
// metric key sets that a later Python renderer consumes; these tests assert
// the LITERAL bytes/keys, not approximate behavior. Five gates:
//   1. SeriesCsvRoundTrips     — write_backtest_series_csv: meta lines, the
//      pinned header string, row count, and a %.17g bit-exact double round-trip.
//   2. MetricsCsv              — write_metrics_csv: exact file bytes.
//   3. StrategyAndSummaryMetrics — strategy_metrics/result_summary_metrics:
//      exact key sets (order pinned) + hand-computed spot values.
//   4. EngineMetrics           — engine_metrics: derived steps_per_s + all keys.
//   5. DbStatsCsv              — write_surface_db_stats_csv against a real
//      on-disk SurfaceDb (surface_db_test.cpp's make_essvi fixture, trimmed to
//      a 1-slice surface): appended db meta + ascending-key-sorted rows.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/run_report.hpp"
#include "atx/vol/surface_archive.hpp" // SurfaceArchiveItem
#include "atx/vol/surface_db.hpp"      // SurfaceDb
#include "atx/vol/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"     // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kArchS = 100.0;
constexpr double kArchR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid) {
  PricingContext pc;
  pc.S = kArchS;
  pc.r = kArchR;
  pc.now_ts_ns = kBaseNow;
  pc.method = AmericanMethod::AndersenLake;
  pc.uid = uid;
  return pc;
}

// Minimal genuine 1-slice eSSVI PricedSurface (surface_db_test.cpp's
// make_essvi, trimmed to n=1 -- "a minimal 1-surface archive per partition"
// per the task brief). Self-contained: no test-only dependency on another
// test binary's translation unit.
[[nodiscard]] PricedSurface make_essvi1(std::uint32_t uid) {
  CurveSurface cs;
  EssviParams e{};
  e.theta = 0.04;
  e.phi = 1.5;
  e.rho = -0.4;
  e.psi = 0.5;
  e.p = 0.5;
  e.lambda = 0.5;
  e.T = 0.25;
  e.F = kArchS;
  e.expiry_id = 0;
  cs.push(std::make_unique<EssviCurve>(e, std::exp(-kArchR * e.T)));
  std::vector<SliceContext> ctx{SliceContext{e.T, e.F, 0.0, 0.02, 250, 7}};
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-run-report-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

[[nodiscard]] std::string read_file(const std::string &path) {
  std::ifstream is(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
}

// Split on '\n'; no trailing empty entry (every writer here ends its output
// with a final '\n', so a well-formed file has none after the last split).
[[nodiscard]] std::vector<std::string> split_lines(const std::string &content) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= content.size()) {
    const std::size_t nl = content.find('\n', start);
    if (nl == std::string::npos) {
      break;
    }
    lines.push_back(content.substr(start, nl - start));
    start = nl + 1;
  }
  return lines;
}

[[nodiscard]] std::vector<std::string> split_csv(const std::string &line) {
  std::vector<std::string> cells;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = line.find(',', start);
    if (comma == std::string::npos) {
      cells.push_back(line.substr(start));
      break;
    }
    cells.push_back(line.substr(start, comma - start));
    start = comma + 1;
  }
  return cells;
}

// The pinned `write_backtest_series_csv` header, verbatim from run_report.hpp.
constexpr const char *kPinnedHeader =
    "date,ts_ns,pnl_total,nav,pnl_delta,pnl_gamma,pnl_vega,pnl_vanna,pnl_volga,"
    "pnl_theta,pnl_rho,pnl_charm,pnl_unexplained,pnl_settlement,pnl_shares,"
    "financing,cost,cash,gross_delta,gross_gamma,gross_vega,gross_theta,"
    "turnover_notional,turnover_vega,n_open_lots,n_unpriced_lots,"
    "n_unpriced_greeks";

// A 3-row hand-built BacktestResult, one signal series, and a pnl_total value
// (0.1 + 0.2) that needs the full 17 significant digits to round-trip.
[[nodiscard]] BacktestResult make_tiny_result() {
  BacktestResult r;
  const std::size_t n = 3;
  for (std::size_t i = 0; i < n; ++i) {
    r.date.push_back("2026-07-0" + std::to_string(i + 1));
    r.ts_ns.push_back(kBaseNow + static_cast<std::int64_t>(i) * kDayNs);
  }
  r.pnl_total = {0.0, 0.1 + 0.2, -1.5};
  r.nav = {0.0, 0.3, -1.2};
  r.pnl_delta = std::vector<double>(n, 1.0);
  r.pnl_gamma = std::vector<double>(n, 2.0);
  r.pnl_vega = std::vector<double>(n, 3.0);
  r.pnl_vanna = std::vector<double>(n, 0.1);
  r.pnl_volga = std::vector<double>(n, 0.2);
  r.pnl_theta = std::vector<double>(n, -0.3);
  r.pnl_rho = std::vector<double>(n, 0.05);
  r.pnl_charm = std::vector<double>(n, -0.02);
  r.pnl_unexplained = std::vector<double>(n, 0.01);
  r.pnl_settlement = std::vector<double>(n, 0.0);
  r.pnl_shares = std::vector<double>(n, -5.0);
  r.financing = std::vector<double>(n, 0.1);
  r.cost = std::vector<double>(n, 0.5);
  r.cash = std::vector<double>(n, 1000.0);
  r.gross_delta = std::vector<double>(n, 10.0);
  r.gross_gamma = std::vector<double>(n, 20.0);
  r.gross_vega = std::vector<double>(n, 100.0);
  r.gross_theta = std::vector<double>(n, -30.0);
  r.turnover_notional = std::vector<double>(n, 500.0);
  r.turnover_vega = std::vector<double>(n, 5.0);
  r.n_open_lots = {0.0, 1.0, 1.0};
  r.n_unpriced_lots = {0.0, 0.0, 1.0};
  r.n_unpriced_greeks = {0.0, 0.0, 0.0};
  r.signals.push_back({"sig_name", {1.5, 2.5, 3.5}});
  return r;
}

[[nodiscard]] std::string find_metric(const MetaKv &kv, const std::string &key) {
  for (const auto &[k, v] : kv) {
    if (k == key) {
      return v;
    }
  }
  ADD_FAILURE() << "missing key: " << key;
  return {};
}

} // namespace

// ── 1. write_backtest_series_csv: meta + pinned header + bit-exact round-trip ──
TEST(RunReport, SeriesCsvRoundTrips) {
  const fs::path dir = fresh_dir("series");
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / "series.csv").string();

  const BacktestResult r = make_tiny_result();
  const MetaKv meta = {{"symbol", "TEST"}, {"strategy", "unit-test"}};
  const Status st = write_backtest_series_csv(r, meta, path);
  ASSERT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());

  const std::string content = read_file(path);
  const std::vector<std::string> lines = split_lines(content);
  ASSERT_EQ(lines.size(), 6u); // 2 meta + 1 header + 3 data rows

  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(lines[i].rfind("# ", 0), 0u) << i;
    EXPECT_NE(lines[i].find('='), std::string::npos) << i;
  }
  EXPECT_EQ(lines[0], "# symbol=TEST");
  EXPECT_EQ(lines[1], "# strategy=unit-test");

  const std::string expected_header = std::string(kPinnedHeader) + ",sig_name";
  EXPECT_EQ(lines[2], expected_header);

  for (int i = 0; i < 3; ++i) {
    const auto cells = split_csv(lines[3 + static_cast<std::size_t>(i)]);
    ASSERT_EQ(cells.size(), 28u); // date,ts_ns + 25 double cols + 1 signal col
  }

  // Row i=1 (data row index 4): pnl_total is column 2 (date=0, ts_ns=1).
  const auto row1 = split_csv(lines[4]);
  const double got = std::stod(row1[2]);
  EXPECT_TRUE(bits_equal(got, 0.1 + 0.2));

  // Signal column (last cell) round-trips too.
  EXPECT_TRUE(bits_equal(std::stod(row1.back()), 2.5));

  std::error_code ec2;
  fs::remove_all(dir, ec2);
}

// ── 2. write_metrics_csv: exact file bytes ──────────────────────────────────
TEST(RunReport, MetricsCsv) {
  const fs::path dir = fresh_dir("metrics");
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / "metrics.csv").string();

  const Status st = write_metrics_csv({{"a", "b"}}, {{"sharpe", "1.25"}}, path);
  ASSERT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());

  EXPECT_EQ(read_file(path), "# a=b\nmetric,value\nsharpe,1.25\n");

  std::error_code ec2;
  fs::remove_all(dir, ec2);
}

// ── 3. strategy_metrics / result_summary_metrics: exact key sets + spot values ──
TEST(RunReport, StrategyAndSummaryMetrics) {
  TearSheet ts;
  ts.total_return = 100.0;
  ts.ann_return = 50.0;
  ts.ann_vol = 20.0;
  ts.sharpe = 2.5;
  ts.max_drawdown = 15.0;
  ts.hit_rate = 0.6;
  ts.avg_turnover = 1000.0;
  ts.total_cost = 5.0;
  ts.total_financing = 2.0;
  ts.attr_delta = 10.0;
  ts.attr_gamma = 20.0;
  ts.attr_vega = 30.0;
  ts.attr_vanna = 1.0;
  ts.attr_volga = 2.0;
  ts.attr_theta = -3.0;
  ts.attr_rho = 0.5;
  ts.attr_charm = -0.2;
  ts.attr_unexplained = 0.1;
  ts.return_on_gross_vega = 0.05;
  ts.vega_adj_sharpe = 1.1;
  ts.pnl_per_vega_traded = 0.25;
  ts.avg_gross_vega = 500.0;
  ts.avg_gross_gamma = 3.0;
  // Fields deliberately NOT in strategy_metrics's key set (must not leak in).
  ts.attr_settlement = 999.0;
  ts.attr_shares = 999.0;
  ts.attr_financing = 999.0;
  ts.attr_cost = 999.0;

  const MetaKv sm = strategy_metrics(ts);
  const std::vector<std::string> expected_keys = {
      "total_return",         "ann_return",      "ann_vol",
      "sharpe",               "max_drawdown",    "hit_rate",
      "avg_turnover",         "total_cost",      "total_financing",
      "attr_delta",           "attr_gamma",      "attr_vega",
      "attr_vanna",           "attr_volga",      "attr_theta",
      "attr_rho",             "attr_charm",      "attr_unexplained",
      "return_on_gross_vega", "vega_adj_sharpe", "pnl_per_vega_traded",
      "avg_gross_vega",       "avg_gross_gamma"};
  ASSERT_EQ(sm.size(), expected_keys.size());
  for (std::size_t i = 0; i < expected_keys.size(); ++i) {
    EXPECT_EQ(sm[i].first, expected_keys[i]) << i;
  }
  EXPECT_DOUBLE_EQ(std::stod(find_metric(sm, "total_return")), 100.0);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(sm, "sharpe")), 2.5);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(sm, "avg_gross_gamma")), 3.0);

  // BacktestResult: 4 rows; n_open_lots is 0 on rows 0 and 2, so avg_net_vega/
  // avg_net_theta must skip those rows' (deliberately extreme) greek values.
  BacktestResult r;
  const std::size_t n = 4;
  for (std::size_t i = 0; i < n; ++i) {
    r.date.push_back("d" + std::to_string(i));
    r.ts_ns.push_back(kBaseNow + static_cast<std::int64_t>(i) * kDayNs);
  }
  r.pnl_total = {0.0, 5.0, -2.0, 3.0};
  r.nav = {0.0, 5.0, 3.0, 6.0};
  r.gross_vega = {0.0, 100.0, 999.0, 300.0};   // 999 at a closed (n_open_lots==0) row
  r.gross_theta = {0.0, -10.0, 999.0, -20.0};  // ditto
  r.n_open_lots = {0.0, 2.0, 0.0, 5.0};        // peak == 5
  r.n_unpriced_lots = {0.0, 1.0, 0.0, 2.0};
  r.n_unpriced_greeks = {0.0, 0.0, 3.0, 0.0};

  const MetaKv rm = result_summary_metrics(r);
  const std::vector<std::string> expected_rkeys = {
      "total_pnl",     "avg_daily_pnl",        "avg_net_vega",         "avg_net_theta",
      "avg_open_lots", "peak_open_lots",       "total_unpriced_lots",  "total_unpriced_greeks",
      "n_steps"};
  ASSERT_EQ(rm.size(), expected_rkeys.size());
  for (std::size_t i = 0; i < expected_rkeys.size(); ++i) {
    EXPECT_EQ(rm[i].first, expected_rkeys[i]) << i;
  }

  EXPECT_DOUBLE_EQ(std::stod(find_metric(rm, "total_pnl")), r.nav.back());
  EXPECT_DOUBLE_EQ(std::stod(find_metric(rm, "peak_open_lots")), 5.0);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(rm, "avg_net_vega")), 200.0);   // (100+300)/2
  EXPECT_DOUBLE_EQ(std::stod(find_metric(rm, "avg_net_theta")), -15.0); // (-10-20)/2
  EXPECT_DOUBLE_EQ(std::stod(find_metric(rm, "avg_daily_pnl")), 2.0);   // (5-2+3)/3
  EXPECT_DOUBLE_EQ(std::stod(find_metric(rm, "avg_open_lots")), 1.75);  // (0+2+0+5)/4
  EXPECT_DOUBLE_EQ(std::stod(find_metric(rm, "total_unpriced_lots")), 3.0);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(rm, "total_unpriced_greeks")), 3.0);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(rm, "n_steps")), 4.0);
}

// ── 4. engine_metrics: derived steps_per_s + all six keys ───────────────────
TEST(RunReport, EngineMetrics) {
  EngineRunStats s;
  s.wall_clock_ms = 2000.0;
  s.n_steps = 10;
  s.cache = SnapshotCacheStats{5, 4, 3};

  const MetaKv em = engine_metrics(s);
  const std::vector<std::string> expected_keys = {"wall_clock_ms", "steps_per_s",  "n_steps",
                                                   "cache_loads",  "cache_hits",   "cache_prefetches"};
  ASSERT_EQ(em.size(), expected_keys.size());
  for (std::size_t i = 0; i < expected_keys.size(); ++i) {
    EXPECT_EQ(em[i].first, expected_keys[i]) << i;
  }

  EXPECT_DOUBLE_EQ(std::stod(find_metric(em, "steps_per_s")), 5.0);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(em, "wall_clock_ms")), 2000.0);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(em, "n_steps")), 10.0);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(em, "cache_loads")), 5.0);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(em, "cache_hits")), 4.0);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(em, "cache_prefetches")), 3.0);
}

// Degenerate wall_clock_ms <= 0 guards steps_per_s to 0 (no div-by-zero/inf).
TEST(RunReport, EngineMetricsGuardsZeroWallClock) {
  EngineRunStats s;
  s.wall_clock_ms = 0.0;
  s.n_steps = 7;
  const MetaKv em = engine_metrics(s);
  EXPECT_DOUBLE_EQ(std::stod(find_metric(em, "steps_per_s")), 0.0);
}

// ── 5. write_surface_db_stats_csv: appended db meta + ascending-key rows ────
TEST(RunReport, DbStatsCsv) {
  const fs::path root = fresh_dir("dbstats");

  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());

  const PricedSurface s1 = make_essvi1(1);
  const PricedSurface s2 = make_essvi1(2);
  const std::vector<SurfaceArchiveItem> items1{{"AAPL", &s1}};
  const std::vector<SurfaceArchiveItem> items2{{"MSFT", &s2}};
  // Written out of ascending order: the writer must sort, not preserve
  // write/manifest order.
  ASSERT_TRUE(db->write_partition("2026-07-02", items2).has_value());
  ASSERT_TRUE(db->write_partition("2026-07-01", items1).has_value());
  ASSERT_EQ(db->partitions().size(), 2u);

  const std::string path = (root / "db_stats.csv").string();
  const MetaKv meta = {{"note", "unit-test"}};
  const Status st = write_surface_db_stats_csv(*db, meta, path);
  ASSERT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());

  const std::string content = read_file(path);
  const std::vector<std::string> lines = split_lines(content);
  // 1 caller meta + 5 appended (db_root,generation,n_symbols,n_partitions,
  // total_file_size) + 1 header + 2 data rows.
  ASSERT_EQ(lines.size(), 9u);

  EXPECT_EQ(lines[0], "# note=unit-test");
  EXPECT_EQ(lines[1].rfind("# db_root=", 0), 0u);
  EXPECT_EQ(lines[2].rfind("# generation=", 0), 0u);
  EXPECT_EQ(lines[3].rfind("# n_symbols=", 0), 0u);
  EXPECT_EQ(lines[4], "# n_partitions=2");
  EXPECT_EQ(lines[5].rfind("# total_file_size=", 0), 0u);
  EXPECT_NE(content.find("# generation="), std::string::npos);
  EXPECT_NE(content.find("# n_partitions=2\n"), std::string::npos);

  EXPECT_EQ(lines[6], "key,surface_count,file_size,created_ts_ns");
  EXPECT_EQ(lines[7].rfind("2026-07-01,", 0), 0u); // sorted ascending by key
  EXPECT_EQ(lines[8].rfind("2026-07-02,", 0), 0u);

  std::error_code ec;
  fs::remove_all(root, ec);
}
