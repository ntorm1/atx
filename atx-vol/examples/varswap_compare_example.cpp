// atx-vol strangle-vs-varswap comparison — the DECLARATIVE example.
//
// The whole strategy is ~20 lines of `StrategySpec`: a fixed-expiry
// daily-restrike 40-delta strangle (FixedContracts 100, delta-hedged daily)
// against one equal-vega uncapped variance swap per cycle, both on the same
// cycle clock. Everything the retired 600-line bespoke driver did with custom
// code — cycle selection, restrike, keep-strikes, swap sizing, accrual-mirror
// signals — is the DSL's job now (strategy.hpp, swap_leg.hpp).
//
//   usage: atx-vol-varswap-compare-example [db_root] [symbol] [out_tsv]
//
// Defaults reproduce the XOM 2026 comparison on the fixed surface db; the
// cumulative-P&L png renders from the TSV with the existing plot script.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp"                 // Clock, RunConfig, UnpricedLotPolicy
#include "atx/vol/api/marketdata/corpus.hpp"                   // CorpusManifest, CorpusEntry
#include "atx/vol/research/backtest_driver.hpp" // run_timed
#include "atx/vol/api/backtest/strategy.hpp"                 // StrategySpec, DeclarativeStrategy
#include "atx/vol/api/storage/surface_archive.hpp"          // SurfaceArchiveV2
#include "atx/vol/api/storage/surface_db.hpp"               // SurfaceDb
#include "atx/vol/tools/tearsheet.hpp"          // write_backtest_tsv
#include "atx/vol/api/core/types.hpp"                    // Result, Status

using namespace atx::vol;

namespace {

constexpr double kDelta = 0.40;
// 91 calendar days (the retired driver's --tenor-days default): ~3M cycles,
// ceil-snapped onto the session grid.
constexpr double kTenorT = 91.0 / 365.25;
constexpr double kContracts = 100.0;

// swap_pv / swap_pnl ride out through the TSV's dynamic signal tail (they are
// deliberately not part of the frozen series-column registry).
[[nodiscard]] Status attach_swap_columns(BacktestResult &r) {
  if (r.swap_pv.size() != r.size() || r.swap_pnl.size() != r.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "swap columns are not row-parallel");
  }
  r.signals.emplace_back("swap_pv", r.swap_pv);
  r.signals.emplace_back("swap_pnl", r.swap_pnl);
  return atx::core::Ok();
}

} // namespace

int main(int argc, char **argv) {
  const std::string db_root = argc > 1 ? argv[1] : "C:/atx-data/surface-db/scratch-fitfix-2026";
  const std::string symbol = argc > 2 ? argv[2] : "XOM";
  const std::string out_tsv = argc > 3 ? argv[3] : "varswap_compare.tsv";

  auto db = SurfaceDb::open(db_root);
  if (!db) {
    std::fprintf(stderr, "SurfaceDb::open(%s): %s\n", db_root.c_str(),
                 db.error().to_string().c_str());
    return 1;
  }
  auto full = Clock::from_surface_db(*db);
  if (!full) {
    std::fprintf(stderr, "Clock::from_surface_db: %s\n", full.error().to_string().c_str());
    return 1;
  }
  // Keep the sessions whose partition carries this symbol (a dark session
  // cannot be stepped over: a live swap fails the run closed on it).
  CorpusManifest live;
  std::vector<std::int64_t> sessions;
  for (const SnapshotRef &ref : full->refs()) {
    auto archive = SurfaceArchiveV2::open_mapped(ref.archive_path);
    if (!archive) {
      std::fprintf(stderr, "%s: %s (inconsistent db)\n", ref.date.c_str(),
                   archive.error().to_string().c_str());
      return 1;
    }
    auto surface = archive->map_symbol(symbol);
    if (!surface) {
      continue; // dark for this symbol
    }
    sessions.push_back(surface->pricing().now_ts_ns);
    live.dates.push_back(ref.date);
    CorpusEntry e;
    e.date = ref.date;
    e.symbol = "*";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = ref.archive_path;
    live.entries.push_back(std::move(e));
  }
  std::sort(sessions.begin(), sessions.end());
  sessions.erase(std::unique(sessions.begin(), sessions.end()), sessions.end());
  auto clock = Clock::from_manifest(live);
  if (!clock) {
    std::fprintf(stderr, "Clock::from_manifest: %s\n", clock.error().to_string().c_str());
    return 1;
  }

  // ── The whole strategy ────────────────────────────────────────────────────
  StrategySpec spec;
  spec.name = symbol + "-strangle-vs-varswap";
  LegSpec strangle;
  strangle.symbol = symbol;
  strangle.tenor.target_T = kTenorT;
  strangle.structure.kind = StructureSpec::Kind::Strangle;
  strangle.structure.call_leg = {StrikeSelector::Kind::Delta, kDelta};
  strangle.structure.put_leg = {StrikeSelector::Kind::Delta, kDelta};
  strangle.size = {SizeSpec::Kind::FixedContracts, kContracts, +1.0};
  spec.legs.push_back(std::move(strangle));
  SwapLegSpec swap;
  swap.symbol = symbol;
  swap.kind = DerivKind::VarSwap;
  swap.size.kind = SwapSizeSpec::Kind::MatchGroupVega; // equal-vega vs the strangle
  spec.swap_legs.push_back(std::move(swap));
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;              // daily restrike
  spec.lifecycle.holding = LifecycleSpec::Holding::FixedExpiryRestrike; // one expiry per cycle
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  spec.session_ts = sessions;
  DeclarativeStrategy strat{std::move(spec)};

  RunConfig rc;
  rc.snapshot_cache = std::make_shared<SnapshotCache>();
  rc.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  rc.reconcile_nav = true;
  auto outcome = run_timed(*clock, strat, rc);
  if (!outcome) {
    std::fprintf(stderr, "run_timed: %s\n", outcome.error().to_string().c_str());
    return 1;
  }
  BacktestResult &r = outcome->result;
  if (const Status st = attach_swap_columns(r); !st) {
    std::fprintf(stderr, "%s\n", st.error().to_string().c_str());
    return 1;
  }
  if (const Status st = write_backtest_tsv(r, out_tsv); !st) {
    std::fprintf(stderr, "write_backtest_tsv: %s\n", st.error().to_string().c_str());
    return 1;
  }

  double swap_total = 0.0;
  for (const double x : r.swap_pnl) {
    swap_total += x;
  }
  const double combined = r.nav.empty() ? 0.0 : r.nav.back();
  std::printf("[%s] sessions=%zu combined=%.2f swap=%.2f strangle=%.2f sharpe=%.3f\n",
              symbol.c_str(), r.size(), combined, swap_total, combined - swap_total,
              outcome->sheet.sharpe);
  std::printf("[counters] skipped_restrikes=%llu unopened=%llu skipped_swaps=%llu\n",
              static_cast<unsigned long long>(strat.skipped_restrikes()),
              static_cast<unsigned long long>(strat.unopened_entry_steps()),
              static_cast<unsigned long long>(strat.skipped_swap_cycles()));
  std::printf("wrote %s (%zu rows)\n", out_tsv.c_str(), r.size());
  return 0;
}
