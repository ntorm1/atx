// atx-vol backtest engine — the design's two worked examples (Phase B3).
//
// Builds a small synthetic eSSVI corpus on disk (no external data), runs each
// example strategy through `run_backtest`, folds the result into a `TearSheet`,
// prints a few headline metrics, and writes the full series to a TSV via
// `write_backtest_tsv`. OFF by default (ATX_BUILD_EXAMPLES); the worked-example
// GATE lives in tests/tearsheet_test.cpp.
//
//   Example A: 3-month ~25-delta SPY put, delta-hedged daily, a new clip each
//              day, held to expiration (EveryStep + HoldToExpiry).
//   Example B: long 9m ~40-delta XOM strangle vs short 3m ~40-delta SPY
//              strangle, flat vega, roll-at-horizon, no hedge.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"         // al_fast_opts, AmericanMethod
#include "atx/vol/api/backtest/backtest.hpp"         // Clock, RunConfig
#include "atx/vol/research/backtest_driver.hpp"  // run_timed (timed engine + tearsheet + stats spine)
#include "atx/vol/api/marketdata/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/api/backtest/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/api/backtest/strategy.hpp"         // DeclarativeStrategy, StrategySpec
#include "atx/vol/api/storage/surface_archive.hpp"  // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/api/fitting/surface_parity.hpp"   // SliceContext
#include "atx/vol/tools/tearsheet.hpp"        // tearsheet, write_backtest_tsv
#include "atx/vol/api/core/types.hpp"            // Side, Result, Status
#include "atx/vol/api/fitting/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/api/fitting/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;

[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd,
                                         std::int64_t now_ts, double vol_bump) {
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
    e.F = fwd;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, fwd, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  if (!ps) {
    std::fprintf(stderr, "make_surface: %s\n", ps.error().to_string().c_str());
    std::exit(1);
  }
  return std::move(*ps);
}

[[nodiscard]] std::string write_archive(
    const fs::path& dir, const std::string& date,
    const std::vector<std::pair<std::string, const PricedSurface*>>& items) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  std::vector<SurfaceArchiveItem> its;
  its.reserve(items.size());
  for (const auto& [sym, ps] : items) {
    its.push_back(SurfaceArchiveItem{sym, ps});
  }
  const Status st = write_surface_archive_v2_file(path, its);
  if (!st) {
    std::fprintf(stderr, "write_archive: %s\n", st.error().to_string().c_str());
    std::exit(1);
  }
  return path;
}

[[nodiscard]] CorpusManifest make_manifest(
    const std::vector<std::pair<std::string, std::string>>& date_paths) {
  CorpusManifest m;
  for (const auto& [date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = "MKT";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

void print_headline(const char* tag, const TearSheet& t) {
  std::printf(
      "[%s] total_return=%.4f ann_return=%.4f ann_vol=%.4f sharpe=%.4f "
      "max_dd=%.4f hit_rate=%.3f\n",
      tag, t.total_return, t.ann_return, t.ann_vol, t.sharpe, t.max_drawdown, t.hit_rate);
  std::printf(
      "[%s] avg_gross_vega=%.2f return_on_gross_vega=%.6f vega_adj_sharpe=%.4f "
      "pnl_per_vega_traded=%.6f total_cost=%.4f\n",
      tag, t.avg_gross_vega, t.return_on_gross_vega, t.vega_adj_sharpe, t.pnl_per_vega_traded,
      t.total_cost);
}

int run_example_a(const fs::path& base) {
  const fs::path dir = base / "exampleA";
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < 12; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double S = 100.0 * (1.0 + 0.004 * static_cast<double>(d));
    const PricedSurface spy = make_surface(7, S, S, now, 0.0008 * static_cast<double>(d));
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    dp.emplace_back(std::string(buf), write_archive(dir, buf, {{"SPY", &spy}}));
  }
  auto clock = Clock::from_manifest(make_manifest(dp));
  if (!clock) {
    std::fprintf(stderr, "clock A: %s\n", clock.error().to_string().c_str());
    return 1;
  }

  StrategySpec spec;
  spec.name = "spy-3m-25d-put-daily-clip";
  LegSpec leg;
  leg.uid = 7;
  leg.tenor.target_T = 0.25;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = StrikeSelector{StrikeSelector::Kind::Delta, 0.25};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  DeclarativeStrategy strat{spec};

  // Spine (5+6+7); `outcome->stats` DISCARDED in both examples — stdout must not move.
  auto outcome = run_timed(*clock, strat);
  if (!outcome) {
    std::fprintf(stderr, "run A: %s\n", outcome.error().to_string().c_str());
    return 1;
  }
  print_headline("Example A", outcome->sheet);
  const std::string tsv = (dir / "example_a.tsv").string();
  const Status st = write_backtest_tsv(outcome->result, tsv);
  if (!st) {
    std::fprintf(stderr, "tsv A: %s\n", st.error().to_string().c_str());
    return 1;
  }
  std::printf("[Example A] wrote %s (%zu rows)\n", tsv.c_str(), outcome->result.size());
  return 0;
}

int run_example_b(const fs::path& base) {
  const fs::path dir = base / "exampleB";
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < 10; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double sx = 110.0 * (1.0 + 0.003 * static_cast<double>(d));
    const double sy = 450.0 * (1.0 + 0.002 * static_cast<double>(d));
    const PricedSurface xom = make_surface(10, sx, sx, now, 0.03);
    const PricedSurface spy = make_surface(20, sy, sy, now, 0.00);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    dp.emplace_back(std::string(buf), write_archive(dir, buf, {{"XOM", &xom}, {"SPY", &spy}}));
  }
  auto clock = Clock::from_manifest(make_manifest(dp));
  if (!clock) {
    std::fprintf(stderr, "clock B: %s\n", clock.error().to_string().c_str());
    return 1;
  }

  const auto strangle = [](std::uint32_t uid, double T, double sign, const char* group) {
    LegSpec leg;
    leg.uid = uid;
    leg.tenor.target_T = T;
    leg.structure.kind = StructureSpec::Kind::Strangle;
    leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.size = SizeSpec{SizeSpec::Kind::Weight, 1.0, sign};
    leg.group = group;
    return leg;
  };

  StrategySpec spec;
  spec.name = "xom9m-vs-spy3m-40d-strangle-flat-vega";
  spec.legs.push_back(strangle(10, 0.75, +1.0, "a"));  // long XOM 9m
  spec.legs.push_back(strangle(20, 0.25, -1.0, "b"));  // short SPY 3m
  spec.constraint = CrossLegConstraint{CrossLegConstraint::Kind::FlatVega, "a", "b"};
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.entry_every_n = 21;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::None, HedgeSpec::Cadence::Daily, 0.0};
  DeclarativeStrategy strat{spec};

  auto outcome = run_timed(*clock, strat);
  if (!outcome) {
    std::fprintf(stderr, "run B: %s\n", outcome.error().to_string().c_str());
    return 1;
  }
  print_headline("Example B", outcome->sheet);
  const std::string tsv = (dir / "example_b.tsv").string();
  const Status st = write_backtest_tsv(outcome->result, tsv);
  if (!st) {
    std::fprintf(stderr, "tsv B: %s\n", st.error().to_string().c_str());
    return 1;
  }
  std::printf("[Example B] wrote %s (%zu rows)\n", tsv.c_str(), outcome->result.size());
  return 0;
}

}  // namespace

int main() {
  const fs::path base = fs::temp_directory_path() / "atx-strategy-examples";
  const int ra = run_example_a(base);
  const int rb = run_example_b(base);
  return (ra == 0 && rb == 0) ? 0 : 1;
}
