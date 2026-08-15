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
#include <string_view>
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

// THE EMISSION IS TESTED, so it lives in a named namespace rather than an
// anonymous one -- exactly as examples/spx_wilmott_repro.cpp does for the
// functions tests/spx_wilmott_repro_test.cpp drives. `main` below is suppressed
// under ATX_VARSWAP_COMPARE_NO_MAIN so tests/varswap_compare_columns_test.cpp
// can link THIS translation unit and call the shipped attach, then diff the
// header of a TSV it really wrote against `swap_explain_columns()`.
//
// WHY THAT TEST EXISTS AND WHAT IT REPLACES. Until fix round 8 the only thing
// standing between this file and a silently truncated attribution tail was a
// Python module that READS THIS SOURCE AS TEXT
// (python/tests/test_render_strangle_vs_varswap.py). A text predicate can only
// ever approximate the property; the round-7 one missed `if (name != "x")` and
// missed `swap_explain_columns().subspan(0, 4)`, both measured. The C++ gate
// observes the artifact instead, so those two holes -- and the ones nobody has
// thought of -- close together.
namespace atx::vol::varswap_compare {

[[nodiscard]] Status attach_one(BacktestResult &r, std::string_view name,
                                const std::vector<double> &column) {
  if (column.size() != r.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "swap column " + std::string(name) +
                              " is not row-parallel (an EMPTY column means the run "
                              "did not compute it; see RunConfig::swap_pnl_explain)");
  }
  r.signals.emplace_back(std::string(name), column);
  return atx::core::Ok();
}

// The swap lane's columns ride out through the TSV's dynamic signal tail (they
// are deliberately not part of the frozen series-column registry).
//
// Task F-8 fix round 4 (R2-I3): the explain tail is DRIVEN from
// `swap_explain_columns()`. This used to be a hand-written table -- the fifth
// copy of that roster, and it predated the header comment claiming there was
// only one. Round 3 could not remove it, because the Python gate derived this
// fixture's signal tail by PARSING the literal rows; round 4 repointed that
// parser at the roster, which is where the names actually live. A ninth column
// now reaches this TSV with no edit to this file.
//
// THE COLUMN NAME IS THE FIELD NAME, and that now holds by construction rather
// than by a hand-kept pairing: `column.name` and `column.member` are the two
// halves of one roster row. The renderer (tools/render_strangle_vs_varswap.py)
// finds the P&L-explain tail by the `swap_explain_` prefix, which is why the
// property matters.
//
// `swap_pv` / `swap_pnl` stay literal: they are the quantity being explained,
// not components of it, they are not on the explain roster, and they lead the
// signal tail in that order.
[[nodiscard]] Status attach_swap_columns(BacktestResult &r) {
  ATX_TRY_VOID(attach_one(r, "swap_pv", r.swap_pv));
  ATX_TRY_VOID(attach_one(r, "swap_pnl", r.swap_pnl));
  for (const BacktestExplainColumn &column : swap_explain_columns()) {
    ATX_TRY_VOID(attach_one(r, column.name, r.*(column.member)));
  }
  return atx::core::Ok();
}

} // namespace atx::vol::varswap_compare

#ifndef ATX_VARSWAP_COMPARE_NO_MAIN

// The strategy shape `main` drives. Inside the guard because it is `main`'s, not
// the emission's: the test target compiles this TU without a `main`, and under
// -Wunused-const-variable an unused constant there is an error.
namespace {

constexpr double kDelta = 0.40;
// 91 calendar days (the retired driver's --tenor-days default): ~3M cycles,
// ceil-snapped onto the session grid.
constexpr double kTenorT = 91.0 / 365.25;
constexpr double kContracts = 100.0;

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
  if (const Status st = atx::vol::varswap_compare::attach_swap_columns(r); !st) {
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

#endif // ATX_VARSWAP_COMPARE_NO_MAIN
