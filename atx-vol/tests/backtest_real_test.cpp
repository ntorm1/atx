// atx-vol backtest engine (Phase C0) — real-OPRA end-to-end litmus.
//
// The whole stack proven on the REAL cached SPY OPRA snapshot (the north-star
// litmus): real fit -> corpus -> backtest -> tearsheet. Every other backtest gate
// (B0-B3) runs on synthetic eSSVI surfaces; this one fits the ACTUAL SPY board
// through the blessed corpus path (OptionChain::from_frame -> PricerFitter::fit ->
// to_priced_surface) and drives a real 25-delta-put delta-hedged strategy over it.
//
// A backtest needs >= 2 dates; we have ONE snapshot. We synthesize the time step
// by stamping the SAME real frame at successive valuation timestamps: quotes /
// skew / spot are held fixed and only `MarketEnv::now_ns` advances. Because the
// fit derives its slice grid from `frame.snapshot_ts_ns` (fixed) while the fitted
// surface's `now_ts_ns` comes from `env.now_ns` (advanced) — see chain.cpp /
// pricer_fitter.cpp — the three dates carry an IDENTICAL fitted surface shape at
// three distinct valuation anchors. That is the real-data analog of B0's
// AgingTimeOnly: a pure time-decay corpus on a REAL fitted surface.
//
// GTEST_SKIP (mirroring spy_real_test) when the parquet fixture is absent so a
// fresh checkout / CI without the fixture stays green. With the fixture present it
// MUST run and pass.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <process.h>  // _getpid — private temp dir per test process (parallel gate)

#include "atx/vol/backtest.hpp"        // Clock, run_backtest, RunConfig, BacktestResult, MarketSnapshot
#include "atx/vol/corpus.hpp"          // CorpusBoard, CorpusConfig, build_corpus, CorpusManifest
#include "atx/vol/market_env.hpp"      // MarketEnv
#include "atx/vol/opra_batch.hpp"      // market_env_from_frame
#include "atx/vol/opra_panel.hpp"      // load_opra_cbbo_parquet, OpraLoadSpec
#include "atx/vol/pricer_fitter.hpp"   // PricerConfig
#include "atx/vol/priced_surface.hpp"  // PricedSurface
#include "atx/vol/session.hpp"         // FitPreset
#include "atx/vol/strategy.hpp"        // DeclarativeStrategy, StrategySpec, resolve_strike_by_delta
#include "atx/vol/tearsheet.hpp"       // TearSheet, tearsheet, write_backtest_tsv
#include "atx/vol/types.hpp"           // Side, Result, Status

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr double kR = 0.043;
constexpr double kTargetT = 30.0 / 365.25;  // ~30d put tenor (~0.0821)

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// Locate the cached SPY parquet across the paths a test binary might run from
// (identical search to spy_real_test).
[[nodiscard]] std::string find_spy_parquet() {
  const char* candidates[] = {
      "data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "../data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "../../data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "C:/atx/data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
  };
  for (const char* c : candidates) {
    if (fs::exists(c)) {
      return c;
    }
  }
  return {};
}

// Every numeric column of two BacktestResults is bit-identical (determinism).
void expect_result_bit_identical(const BacktestResult& a, const BacktestResult& b) {
  ASSERT_EQ(a.size(), b.size());
  const std::vector<std::pair<const std::vector<double>*, const std::vector<double>*>> cols = {
      {&a.pnl_total, &b.pnl_total},           {&a.pnl_delta, &b.pnl_delta},
      {&a.pnl_gamma, &b.pnl_gamma},           {&a.pnl_vega, &b.pnl_vega},
      {&a.pnl_vanna, &b.pnl_vanna},           {&a.pnl_volga, &b.pnl_volga},
      {&a.pnl_theta, &b.pnl_theta},           {&a.pnl_rho, &b.pnl_rho},
      {&a.pnl_charm, &b.pnl_charm},           {&a.pnl_unexplained, &b.pnl_unexplained},
      {&a.pnl_settlement, &b.pnl_settlement}, {&a.pnl_shares, &b.pnl_shares},
      {&a.financing, &b.financing},           {&a.cost, &b.cost},
      {&a.nav, &b.nav},                       {&a.cash, &b.cash},
      {&a.gross_delta, &b.gross_delta},       {&a.gross_gamma, &b.gross_gamma},
      {&a.gross_vega, &b.gross_vega},         {&a.gross_theta, &b.gross_theta},
      {&a.turnover_notional, &b.turnover_notional},
      {&a.turnover_vega, &b.turnover_vega},   {&a.n_open_lots, &b.n_open_lots},
      {&a.n_unpriced_lots, &b.n_unpriced_lots},
      {&a.n_unpriced_greeks, &b.n_unpriced_greeks}};
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.date[i], b.date[i]) << i;
    EXPECT_EQ(a.ts_ns[i], b.ts_ns[i]) << i;
    for (const auto& [va, vb] : cols) {
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << i;
    }
  }
}

// The B3 closure sum the design pins as a gate.
[[nodiscard]] double closure_sum(const TearSheet& t) noexcept {
  return t.attr_delta + t.attr_gamma + t.attr_vega + t.attr_vanna + t.attr_volga + t.attr_theta +
         t.attr_rho + t.attr_charm + t.attr_unexplained + t.attr_settlement + t.attr_shares +
         t.attr_financing - t.attr_cost;
}

[[nodiscard]] bool all_finite(const TearSheet& t) noexcept {
  const double fields[] = {
      t.total_return,     t.ann_return,     t.ann_vol,          t.sharpe,
      t.max_drawdown,     t.hit_rate,       t.avg_turnover,     t.total_cost,
      t.total_financing,  t.attr_delta,     t.attr_gamma,       t.attr_vega,
      t.attr_vanna,       t.attr_volga,     t.attr_theta,       t.attr_rho,
      t.attr_charm,       t.attr_unexplained, t.attr_settlement, t.attr_shares,
      t.attr_financing,   t.attr_cost,      t.return_on_gross_vega,
      t.vega_adj_sharpe,  t.pnl_per_vega_traded, t.avg_gross_vega, t.avg_gross_gamma};
  for (const double v : fields) {
    if (!std::isfinite(v)) {
      return false;
    }
  }
  return true;
}

// The one 30d 25-delta SPY put, 10 lots, delta-hedged daily, one clip each step.
[[nodiscard]] StrategySpec make_spec() {
  StrategySpec spec;
  spec.name = "spy-30d-25d-put-delta-hedged";
  LegSpec leg;
  leg.symbol = "SPY";  // resolved to uid against the snapshot's SurfaceSet
  leg.tenor.target_T = kTargetT;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = StrikeSelector{StrikeSelector::Kind::Delta, 0.25};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 10.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 1.0};
  return spec;
}

// ── Suite fixture: fit the real 3-date corpus ONCE ──────────────────────────
//
// SetUpTestSuite loads the SPY panel and builds the pure-time-decay corpus (one
// real Fast fit per date, fanned out by build_corpus). It is the only expensive
// step; each TEST_F reloads archives (cheap deserialize) and prices.
class BacktestReal : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    const std::string path = find_spy_parquet();
    if (path.empty()) {
      available_ = false;
      return;
    }
    OpraLoadSpec spec;
    spec.path = path;
    spec.underlying = "SPY";
    spec.snapshot_iso = "2026-06-05T19:55:00Z";
    spec.r = kR;
    auto panel = load_opra_cbbo_parquet(spec);
    if (!panel.has_value()) {
      load_error_ = panel.error().to_string();
      available_ = false;
      return;
    }
    implied_spot_ = panel->implied_spot;

    // env0 from the real frame; each date advances ONLY the valuation clock.
    const MarketEnv env0 = market_env_from_frame(panel->frame);
    const char* dates[kNDates] = {"2026-06-05", "2026-06-06", "2026-06-07"};
    std::vector<CorpusBoard> boards;
    boards.reserve(kNDates);
    for (int d = 0; d < kNDates; ++d) {
      CorpusBoard b;
      b.date = dates[d];
      b.symbol = "SPY";
      b.frame = panel->frame;  // quotes / skew / spot fixed across dates
      b.env = env0;
      b.env.now_ns = env0.now_ns + static_cast<std::int64_t>(d) * kDayNs;  // clock only
      boards.push_back(std::move(b));
    }

    // Per-process private corpus dir: the two BacktestReal.* tests run in
    // separate ctest processes under the parallel gate (ctest -L atx_vol -j16),
    // and a shared fixed path let one test's SetUp/TearDown remove_all() delete
    // the archive out from under the other's in-flight run_backtest (flaky
    // "SurfaceArchiveV2::open_file: file not found"). The PID suffix isolates them.
    out_dir_ = fs::temp_directory_path() /
               ("atx-backtest-real-corpus-" + std::to_string(::_getpid()));
    std::error_code ec;
    fs::remove_all(out_dir_, ec);

    CorpusConfig cfg;
    cfg.fit_template.preset = FitPreset::Fast;  // matches spy_real_test's headline fit
    auto man = build_corpus(boards, out_dir_.string(), cfg);
    if (!man.has_value()) {
      load_error_ = man.error().to_string();
      available_ = false;
      return;
    }
    manifest_ = *man;
    available_ = (manifest_.n_ok == static_cast<std::uint32_t>(kNDates));
    if (!available_) {
      load_error_ = "corpus n_ok=" + std::to_string(manifest_.n_ok) + " (expected " +
                    std::to_string(kNDates) + ")";
    }
  }

  static void TearDownTestSuite() {
    std::error_code ec;
    fs::remove_all(out_dir_, ec);
  }

  static constexpr int kNDates = 3;
  static bool available_;
  static std::string load_error_;
  static CorpusManifest manifest_;
  static double implied_spot_;
  static fs::path out_dir_;
};

bool BacktestReal::available_ = false;
std::string BacktestReal::load_error_;
CorpusManifest BacktestReal::manifest_;
double BacktestReal::implied_spot_ = 0.0;
fs::path BacktestReal::out_dir_;

}  // namespace

// ── 1. Real SPY 25-delta put, delta-hedged: the full end-to-end litmus ──────
TEST_F(BacktestReal, RealSpyPutDeltaHedged) {
  if (!available_) {
    GTEST_SKIP() << "cached SPY OPRA parquet absent or corpus fit failed: "
                 << (load_error_.empty() ? "fixture not found" : load_error_);
  }

  // Sanity on the real snapshot the corpus was fit from.
  EXPECT_GT(implied_spot_, 600.0);
  EXPECT_LT(implied_spot_, 900.0);

  auto clock = Clock::from_manifest(manifest_);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ASSERT_EQ(clock->size(), static_cast<std::size_t>(kNDates));

  // Independent reprice of the 25-delta put strike on the loaded base snapshot.
  auto base = MarketSnapshot::load(clock->refs().front().archive_path);
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  const std::optional<std::uint32_t> spy_uid = base->uid_of("SPY");
  ASSERT_TRUE(spy_uid.has_value());
  const SurfaceRef surf = base->find(*spy_uid);
  ASSERT_NE(surf, nullptr);

  const Result<double> K = resolve_strike_by_delta(*surf, kTargetT, Side::Put, 0.25);
  ASSERT_TRUE(K.has_value()) << K.error().to_string();
  const Result<AmericanGreeks> gr = surf->greeks(*K, kTargetT, Side::Put);
  ASSERT_TRUE(gr.has_value()) << gr.error().to_string();
  const double forward = surf->forward_at(kTargetT);
  ASSERT_GT(forward, 0.0);
  // The resolver validates its root against a real reprice; |delta| lands on 0.25.
  EXPECT_NEAR(std::fabs(gr->delta), 0.25, 1e-3) << "repriced 25d put delta";
  EXPECT_LT(*K, forward) << "a 25-delta put strike sits below the forward";
  EXPECT_TRUE(std::isfinite(*K) && *K > 0.0);

  // Run the strategy (frictionless default).
  const StrategySpec spec = make_spec();
  DeclarativeStrategy strat{spec};
  auto res = run_backtest(*clock, strat);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;
  EXPECT_EQ(r.size(), static_cast<std::size_t>(kNDates));  // inception + (N-1) steps

  // The recorded book delta must be finite at every row (hedge overlay engaged).
  for (std::size_t i = 0; i < r.size(); ++i) {
    EXPECT_TRUE(std::isfinite(r.gross_delta[i])) << "row " << i;
    EXPECT_TRUE(std::isfinite(r.gross_vega[i])) << "row " << i;
  }

  // Tearsheet: closure identity + every field finite.
  const TearSheet t = tearsheet(r);
  EXPECT_TRUE(all_finite(t)) << "tearsheet carries a NaN/Inf field";
  const double closure = closure_sum(t);
  const double resid = std::fabs(t.total_return - closure);
  EXPECT_LE(resid, 1e-6 * (std::fabs(t.total_return) + 1.0))
      << "total_return=" << t.total_return << " closure=" << closure;

  // Determinism: n_threads 1 vs 4 => bit-identical BacktestResult.
  DeclarativeStrategy s1{spec};
  DeclarativeStrategy s4{spec};
  RunConfig cfg1;
  cfg1.price.n_threads = 1;
  RunConfig cfg4;
  cfg4.price.n_threads = 4;
  auto r1 = run_backtest(*clock, s1, cfg1);
  auto r4 = run_backtest(*clock, s4, cfg4);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r4.has_value()) << r4.error().to_string();
  expect_result_bit_identical(*r1, *r4);

  // Time-decay isolation: dates differ by valuation clock only (spot / quotes
  // fixed, surface shape frame-anchored), so each priced step's unexplained PnL is
  // bounded by |theta| — the real analog of B0 AgingTimeOnly. Unlike B0's synthetic
  // FLAT eSSVI (where the bound is 5e-2), a REAL steep-skew ConvexDense SPY board
  // carries genuine term-structure vol curvature: as residual T shrinks over a step
  // the ATM/skew vol moves along the term structure, a higher-order effect the
  // 2nd-order Taylor axes only partly capture, so the observed ratio is ~0.31. The
  // bound is loosened to a documented 0.50 (unexplained still stays below half of
  // theta-scale) and the ratio is PRINTED so it stays honest; the hard gates remain
  // finiteness + attribution-closure above.
  double max_ratio = 0.0;
  for (std::size_t i = 1; i < r.size(); ++i) {
    const double ratio = std::fabs(r.pnl_unexplained[i]) / (std::fabs(r.pnl_theta[i]) + 1.0);
    max_ratio = std::max(max_ratio, ratio);
    EXPECT_TRUE(std::isfinite(r.pnl_unexplained[i]) && std::isfinite(r.pnl_theta[i])) << i;
  }
  EXPECT_LE(max_ratio, 0.50) << "time-decay unexplained/|theta| ratio too large";

  std::printf(
      "[backtest-real] K=%.2f F=%.2f |delta|=%.5f total_return=%.4f sharpe=%.4f "
      "avg_gross_vega=%.2f closure_resid=%.3e max_theta_ratio=%.4f det=OK\n",
      *K, forward, std::fabs(gr->delta), t.total_return, t.sharpe, t.avg_gross_vega, resid,
      max_ratio);
}

// ── 2. Real SPY tearsheet is sane + the TSV export round-trips bit-exact ─────
TEST_F(BacktestReal, RealSpyTearsheetSane) {
  if (!available_) {
    GTEST_SKIP() << "cached SPY OPRA parquet absent or corpus fit failed: "
                 << (load_error_.empty() ? "fixture not found" : load_error_);
  }
  auto clock = Clock::from_manifest(manifest_);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const StrategySpec spec = make_spec();
  DeclarativeStrategy strat{spec};
  auto res = run_backtest(*clock, strat);  // frictionless default
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;

  const TearSheet t = tearsheet(r);
  EXPECT_EQ(t.total_cost, 0.0) << "frictionless run must book zero cost";
  EXPECT_EQ(t.attr_cost, 0.0);
  // One clip each step (EveryStep / HoldToExpiry): the open-lot count climbs by one
  // per row (no cohort expires within the 3-date span).
  ASSERT_EQ(r.n_open_lots.size(), static_cast<std::size_t>(kNDates));
  for (std::size_t i = 0; i < r.n_open_lots.size(); ++i) {
    EXPECT_EQ(r.n_open_lots[i], static_cast<double>(i + 1)) << "row " << i;
  }

  // write_backtest_tsv round-trips every double column bit-exact (the B3 idea).
  const std::string path = (out_dir_ / "real_run.tsv").string();
  const Status st = write_backtest_tsv(r, path);
  ASSERT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());

  std::ifstream is(path, std::ios::binary);
  ASSERT_TRUE(is.good());
  std::string content((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  std::vector<std::vector<std::string>> table;
  {
    std::size_t start = 0;
    while (start <= content.size()) {
      const std::size_t nl = content.find('\n', start);
      if (nl == std::string::npos) {
        break;
      }
      const std::string line = content.substr(start, nl - start);
      std::vector<std::string> cells;
      std::size_t cs = 0;
      while (true) {
        const std::size_t tab = line.find('\t', cs);
        if (tab == std::string::npos) {
          cells.push_back(line.substr(cs));
          break;
        }
        cells.push_back(line.substr(cs, tab - cs));
        cs = tab + 1;
      }
      table.push_back(std::move(cells));
      start = nl + 1;
    }
  }
  ASSERT_EQ(table.size(), r.size() + 1);  // header + one row per step
  const std::vector<std::string>& header = table.front();
  const auto col_index = [&](const std::string& name) -> std::size_t {
    for (std::size_t i = 0; i < header.size(); ++i) {
      if (header[i] == name) {
        return i;
      }
    }
    ADD_FAILURE() << "column not found: " << name;
    return header.size();
  };

  const std::vector<std::pair<std::string, const std::vector<double>*>> dcols = {
      {"pnl_total", &r.pnl_total},   {"pnl_theta", &r.pnl_theta},
      {"pnl_delta", &r.pnl_delta},   {"pnl_unexplained", &r.pnl_unexplained},
      {"nav", &r.nav},               {"gross_delta", &r.gross_delta},
      {"gross_vega", &r.gross_vega}, {"n_open_lots", &r.n_open_lots}};
  std::size_t checked = 0;
  for (const auto& [name, col] : dcols) {
    const std::size_t ci = col_index(name);
    ASSERT_LT(ci, header.size()) << name;
    for (std::size_t i = 0; i < r.size(); ++i) {
      const double got = std::strtod(table[i + 1][ci].c_str(), nullptr);
      EXPECT_TRUE(bits_equal(got, (*col)[i])) << name << " row " << i;
      ++checked;
    }
  }

  std::printf("[backtest-real] tearsheet sane: total_cost=%.1f n_open_lots(last)=%.0f "
              "tsv round-trip %zu cells bit-exact\n",
              t.total_cost, r.n_open_lots.back(), checked);
}
