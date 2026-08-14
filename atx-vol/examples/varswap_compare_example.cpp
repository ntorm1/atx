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

#include "atx/vol/backtest.hpp"                 // Clock, RunConfig, UnpricedLotPolicy
#include "atx/vol/corpus.hpp"                   // CorpusManifest, CorpusEntry
#include "atx/vol/research/backtest_driver.hpp" // run_timed
#include "atx/vol/strategy.hpp"                 // StrategySpec, DeclarativeStrategy
#include "atx/vol/surface_archive.hpp"          // SurfaceArchiveV2
#include "atx/vol/surface_db.hpp"               // SurfaceDb
#include "atx/vol/tools/tearsheet.hpp"          // write_backtest_tsv
#include "atx/vol/types.hpp"                    // Result, Status

using namespace atx::vol;

namespace {

constexpr double kDelta = 0.40;
// 91 calendar days (the retired driver's --tenor-days default): ~3M cycles,
// ceil-snapped onto the session grid.
constexpr double kTenorT = 91.0 / 365.25;
constexpr double kContracts = 100.0;

// The swap lane's columns ride out through the TSV's dynamic signal tail (they
// are deliberately not part of the frozen series-column registry). THE COLUMN
// NAME IS THE FIELD NAME on every row: the renderer
// (tools/render_strangle_vs_varswap.py) finds the P&L-explain tail by its
// `swap_explain_` prefix, and its gate test parses this table against
// backtest.hpp's own declarations, so a column that lands on one side only is a
// red test rather than a column silently missing from every report.
[[nodiscard]] Status attach_swap_columns(BacktestResult &r) {
  const std::pair<const char *, const std::vector<double> *> columns[] = {
      {"swap_pv", &r.swap_pv},
      {"swap_pnl", &r.swap_pnl},
      {"swap_explain_carry", &r.swap_explain_carry},
      {"swap_explain_realized", &r.swap_explain_realized},
      {"swap_explain_vol_level", &r.swap_explain_vol_level},
      {"swap_explain_skew", &r.swap_explain_skew},
      {"swap_explain_convexity", &r.swap_explain_convexity},
      {"swap_explain_discount", &r.swap_explain_discount},
      {"swap_explain_residual", &r.swap_explain_residual},
      {"swap_explain_unattributed", &r.swap_explain_unattributed},
  };
  // R2-I3, PARTIALLY addressed -- read this before assuming the table above is
  // free-standing. It is a hand-written copy of `swap_explain_columns()`, and it
  // could NOT be driven from that roster in fix round 3.
  //
  // Driving it was attempted and reverted, with the measurement:
  // `test_render_strangle_vs_varswap.py` derives this fixture's signal tail by
  // PARSING these `{"name", &r.field}` rows (`example_attached_columns`, which
  // requires `text.startswith('{"')`). Replacing them with a loop over the
  // roster leaves nothing to parse, and that module raises at IMPORT time -- all
  // eight of its tests fail to collect, not one assertion. The gate is behaving
  // exactly as designed; the fix belongs in the Python lane, repointing that
  // parser at the roster, and is a two-lane change.
  //
  // What IS closed here: the size coupling. A ninth roster column now breaks
  // THIS build rather than only the cross-language gate, so the copy cannot
  // silently fall behind even if the Python lane is never run.
  static_assert(std::size(columns) == swap_explain_column_count() + 2u,
                "this attach table is a hand-written mirror of swap_explain_columns() plus "
                "swap_pv/swap_pnl; the roster gained or lost a column, so add or remove the "
                "matching {\"name\", &r.field} row here (it cannot be driven from the roster "
                "-- atx-vol/python parses these rows, see the note above)");
  for (const auto &[name, column] : columns) {
    if (column->size() != r.size()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            std::string("swap column ") + name +
                                " is not row-parallel (an EMPTY column means the run "
                                "did not compute it; see RunConfig::swap_pnl_explain)");
    }
    r.signals.emplace_back(name, *column);
  }
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
  // OFF by default in the library — it costs up to 20 repricings per live lot
  // per step — but this example exists to produce the comparison report, and
  // attributing `swap_pnl` is what separates a bad day from a bad model there.
  // One swap lot is live per cycle, so the bill is one lot's worth of repricing.
  rc.swap_pnl_explain = true;
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
