// bev_label_factory_gate_test.cpp — correctness GATE for the label-factory
// driver (THEO-6).
//
// `run_bev_label_factory` / `BevFactoryArgs` have no separate header (the
// driver's own banner comment explains why: they are a small, CLI-only arg
// bag with no reuse value beyond this one driver). So this test pulls the
// EXACT implementation the shipped example TU runs by #include-ing
// examples/bev_label_factory.cpp directly (its `main()` suppressed via
// ATX_BEV_LABEL_FACTORY_NO_MAIN) rather than reimplementing or forward-
// declaring a second, ODR-risky copy of the struct.
//
// Self-contained (synthetic eSSVI surfaces via the spy_strangle_backtest_
// test.cpp `make_surface` pattern; no fit, no external data): builds a small
// SurfaceDb corpus, then runs the driver TWICE with IDENTICAL args into two
// temp files and pins:
//   (a) both runs exit Ok(0);
//   (b) the two output files are BYTE-IDENTICAL (memcmp, whole file);
//   (c) at least one row converged (flag == 0, i.e. BevFlag::Ok);
//   (d) every written row's log_ratio is finite.

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
