// spy_leaps_strangle_backtest.cpp — long-dated SPY strangle acceptance driver.
//
// One 40-delta SPY strangle entered at a ~2-year tenor, rolled every calendar
// month (close + re-strike at the fresh tenor), delta-hedged to zero every
// session, zero frictions. The timeline spans MULTIPLE per-year SurfaceDb
// roots (spy-2019 .. spy-2026), which no single `Clock::from_surface_db` can
// produce — so the driver assembles one `CorpusManifest` across the roots and
// takes the cross-year clock from `Clock::from_manifest`. Emits the
// self-describing PnL-track TSV plus the full per-step series TSV.
//
//   atx-vol-spy-leaps-strangle --out DIR
//       [--db-prefix C:/atx-data/surface-db-r2/spy] [--year-lo 2019] [--year-hi 2026]
//       [--from YYYY-MM-DD] [--to YYYY-MM-DD]
//       [--delta 0.40] [--tenor-years 2.0] [--roll-months 1] [--contracts 1]
//       [--sign +1|-1]
//
// Flow: parse -> open each per-year root -> manifest + session-ts grid ->
// windowed Clock -> DeclarativeStrategy (Strangle/Delta/RollAtHorizon/
// DeltaToZero-daily) -> run_backtest -> track.tsv + series.tsv -> summary.
// Exit codes: 2 bad args, 1 runtime error. OFF by default (ATX_BUILD_EXAMPLES);
// acceptance driver, not a shipped operator CLI.
//
// The monthly roll is expressed through the DSL's residual-tenor trigger:
// `roll_at_T = tenor_years - roll_months/12`, so the single RollAtHorizon
// cohort is closed at marks and re-opened at the fresh 2y strikes as soon as
// one calendar month of tenor has decayed. Contracts, strikes and expiries are
// synthetic surface coordinates (frictionless model book), with expiries
// snapped onto the run's own session grid so a cohort's anchor is always an
// observable session.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp"        // Clock, RunConfig, SnapshotCache, run_backtest
#include "atx/vol/api/marketdata/corpus.hpp"          // CorpusManifest, CorpusEntry
#include "analytics/realized_vol.hpp"    // OhlcBar, RvPanel, RvEstimator, realized_vol_panel
#include "atx/vol/api/backtest/strategy.hpp"        // StrategySpec, DeclarativeStrategy
#include "atx/vol/api/storage/surface_db.hpp"      // SurfaceDb
#include "pricing/theo.hpp"            // TheoEngine, TheoContext, TheoQuery, make_rv_blend_overlay
#include "atx/vol/tools/tearsheet.hpp" // TearSheet, tearsheet, write_backtest_* TSV
#include "atx/vol/api/core/types.hpp"           // Result, Status

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

using Meta = std::vector<std::pair<std::string, std::string>>;

struct Args {
  std::string db_prefix{"C:/atx-data/surface-db-r2/spy"};
  int year_lo{2019};
  int year_hi{2026};
  std::string from;
  std::string to;
  std::string out;
  double delta{0.40};
  double tenor_years{2.0};
  double roll_months{1.0};
  double contracts{1.0};
  double sign{+1.0};
  std::string mark_domain{"extrapolate"};
  // Task 10: read-only theo-edge signal probe. OFF by default (final-review
  // I3: the original default-ON wiring silently stopped the driver's own
  // default runs from exercising the default zero-copy WS-ZC1 borrow path --
  // see main()'s comment). `--theo-signals` opts in; the A/B NAV-byte-
  // identity proof (Task 10 brief, step 4) needs both variants out of the
  // SAME binary/build, so this stays a flag rather than two binaries.
  bool emit_theo_signals{false};
};

void usage() {
  std::fprintf(stderr,
               "usage: atx-vol-spy-leaps-strangle --out DIR [--db-prefix P] [--year-lo Y] "
               "[--year-hi Y]\n    [--from D] [--to D] [--delta X] [--tenor-years X] "
               "[--roll-months X] [--contracts X] [--sign +1|-1]\n    "
               "[--mark-domain extrapolate|carry|error] [--theo-signals] "
               "[--no-theo-signals]\n"
               "    (theo signals: OFF by default, so the default (no-flag) run exercises "
               "the default zero-copy WS-ZC1 borrow path. --theo-signals opts in and forces "
               "owned archive backing instead, ATX_VOL_ZC_BORROW=0, ONLY for this run -- "
               "see main(); --no-theo-signals is accepted as a no-op (OFF is already the "
               "default). Assumes a SPY corpus regardless of --db-prefix; "
               "theo_edge_atm/theo_band_atm are NaN for any other symbol.)\n");
}

bool parse_args(int argc, char **argv, Args &args) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag{argv[i]};
    auto next = [&](std::string &dst) {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    std::string v;
    if (flag == "--out" && next(v)) {
      args.out = v;
    } else if (flag == "--db-prefix" && next(v)) {
      args.db_prefix = v;
    } else if (flag == "--year-lo" && next(v)) {
      args.year_lo = std::atoi(v.c_str());
    } else if (flag == "--year-hi" && next(v)) {
      args.year_hi = std::atoi(v.c_str());
    } else if (flag == "--from" && next(v)) {
      args.from = v;
    } else if (flag == "--to" && next(v)) {
      args.to = v;
    } else if (flag == "--delta" && next(v)) {
      args.delta = std::atof(v.c_str());
    } else if (flag == "--tenor-years" && next(v)) {
      args.tenor_years = std::atof(v.c_str());
    } else if (flag == "--roll-months" && next(v)) {
      args.roll_months = std::atof(v.c_str());
    } else if (flag == "--contracts" && next(v)) {
      args.contracts = std::atof(v.c_str());
    } else if (flag == "--sign" && next(v)) {
      args.sign = std::atof(v.c_str());
    } else if (flag == "--mark-domain" && next(v)) {
      args.mark_domain = v;
    } else if (flag == "--theo-signals") {
      args.emit_theo_signals = true;
    } else if (flag == "--no-theo-signals") {
      args.emit_theo_signals = false; // no-op: OFF is already the default (I3)
    } else {
      std::fprintf(stderr, "unknown/incomplete flag: %.*s\n", static_cast<int>(flag.size()),
                   flag.data());
      return false;
    }
  }
  if (args.out.empty()) {
    std::fprintf(stderr, "--out is required\n");
    return false;
  }
  if (args.year_lo > args.year_hi || args.delta <= 0.0 || args.delta >= 1.0 ||
      args.tenor_years <= 0.0 || args.roll_months <= 0.0 ||
      args.roll_months / 12.0 >= args.tenor_years || args.contracts <= 0.0 ||
      (args.sign != 1.0 && args.sign != -1.0)) {
    std::fprintf(stderr, "invalid argument values\n");
    return false;
  }
  if (args.mark_domain != "extrapolate" && args.mark_domain != "carry" &&
      args.mark_domain != "error") {
    std::fprintf(stderr, "--mark-domain must be extrapolate|carry|error\n");
    return false;
  }
  return true;
}

char num_buf[64];
std::string fmt_num(double v) {
  std::snprintf(num_buf, sizeof num_buf, "%.10g", v);
  return num_buf;
}

// ── Task 10: read-only theo-edge backtest signal probe ─────────────────────
//
// Decorates the strangle strategy driving this backtest with two per-step
// diagnostic columns, `theo_edge_atm` / `theo_band_atm`, from a
// `TheoEngine{RvBlend}` (default `RvBlendConfig`) over the step's own SPY
// surface. Mirrors `SwapSignalProbe`'s split (swap_leg.hpp): a non-const
// step that mirrors state the engine doesn't expose (there, per-lot swap
// accrual; here, a rolling realized-vol history this driver has no other
// reason to keep), and a const `signals()` reader that consumes it.
//
// READ-ONLY BY CONSTRUCTION: every `IStrategy` virtual except `signals()`
// forwards UNCHANGED to `inner_` -- this type adds no order, hedge, or NAV
// path of its own, only an extra diagnostic column the engine records
// alongside the ones the inner strategy already drives. `on_step` mirrors
// realized-vol history from `base` (a READ), never touches `book` beyond
// forwarding it to `inner_.on_step` untouched, so the NAV/book/hedge
// trajectory this driver produces is IDENTICAL, byte-for-byte, whether or not
// this wrapper is in the loop (Task 10 brief step 4's non-negotiable proof).
class TheoEdgeSignalStrategy final : public IStrategy {
public:
  TheoEdgeSignalStrategy(IStrategy &inner, TheoEngine engine, std::string underlier_symbol,
                         double atm_tenor_years)
      : inner_(inner), engine_(std::move(engine)), symbol_(std::move(underlier_symbol)),
        tenor_years_(atm_tenor_years) {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    update_rv_history(base);
    return inner_.on_step(base, step_index, book, next_lot_id);
  }
  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id, const PriceOptions &price_options) override {
    update_rv_history(base);
    return inner_.on_step(base, step_index, book, next_lot_id, price_options);
  }
  [[nodiscard]] std::span<const FullGreekSeed> entry_risk_seeds() const noexcept override {
    return inner_.entry_risk_seeds();
  }
  [[nodiscard]] HedgeSpec hedge_spec() const override { return inner_.hedge_spec(); }
  [[nodiscard]] QueryExecution required_economic_execution() const noexcept override {
    return inner_.required_economic_execution();
  }
  [[nodiscard]] std::span<const std::uint32_t> referenced_uids() const noexcept override {
    return inner_.referenced_uids();
  }

  // theo_edge_atm / theo_band_atm, ALWAYS both, from the RvBlend-overlaid
  // TheoEngine at the ATM (K == spot), tenor-matched (T == the strategy's own
  // target LEAPS tenor) query on the step's SPY surface. NaN when the SPY
  // surface can't be resolved on `base` or the TheoEngine call itself fails
  // (never a missing column -- mirrors SwapSignalProbe's "absent measurement
  // is NaN, never a missing column" contract, swap_leg.hpp). NOTE (final-
  // review M1): `theo_band_atm` is the engine's config floor
  // (`TheoConfig::band_floor_vol`, 0.002) on every resolved step here --
  // `RvBlendOverlay`, the only overlay this driver engages, always reports
  // `band = 0`, so nothing on this path ever drives it above the floor; read
  // it as "config floor", not a live per-step uncertainty estimate.
  [[nodiscard]] std::vector<std::pair<std::string, double>>
  signals(const MarketSnapshot &base) const override {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    double edge_atm = kNaN;
    double band_atm = kNaN;
    const PricedSurface *surface = spy_surface(base);
    if (surface != nullptr && !bars_.empty()) {
      // CloseToClose over this driver's own degenerate (O=H=L=C=spot) daily
      // bars -- see update_rv_history: no intraday range is observable from a
      // once-daily surface snapshot, so an estimator that needs a real
      // high/low range (Parkinson/Garman-Klass/Rogers-Satchell/YangZhang)
      // would be reading a structurally-zero range every day, not a genuine
      // absence of movement.
      const Result<RvPanel> panel = realized_vol_panel(bars_, RvEstimator::CloseToClose);
      const TheoContext ctx{.surface = surface, .rv = panel.has_value() ? &*panel : nullptr};
      const TheoQuery q{
          .strike = surface->pricing().S, .tenor_years = tenor_years_, .side = Side::Call};
      const Result<TheoValue> v = engine_.value(ctx, q);
      if (v.has_value()) {
        edge_atm = v->edge_vol;
        band_atm = v->band_vol;
      }
    }
    return {{"theo_edge_atm", edge_atm}, {"theo_band_atm", band_atm}};
  }

private:
  // Owned-surface lookup for `symbol_` on `base`, or nullptr (unknown symbol,
  // or a view-backed/borrowed snapshot -- `SurfaceRef::owned()` is null on the
  // BORROW route MarketSnapshot::load takes by default at this driver's
  // QueryPricingTier::LegacyCompatible, see backtest.cpp's WS-ZC1 comment;
  // `main()`'s `ATX_VOL_ZC_BORROW=0` is what makes this resolve non-null here).
  [[nodiscard]] const PricedSurface *spy_surface(const MarketSnapshot &base) const noexcept {
    const std::optional<std::uint32_t> uid = base.uid_of(symbol_);
    if (!uid.has_value()) {
      return nullptr;
    }
    return base.find(*uid).owned();
  }

  // Mirrors one degenerate daily OHLC bar (O=H=L=C=spot) per step into a
  // rolling, unbounded history -- this driver's own state, kept ONLY because
  // the RvBlend overlay needs an RvPanel and nothing upstream of this wrapper
  // computes one from an options-surface backtest's daily spot series. A
  // non-finite/non-positive spot, or a repeated timestamp (defensive; the
  // engine calls on_step exactly once per step), is skipped rather than
  // corrupting the history with a bad or duplicate bar.
  void update_rv_history(const MarketSnapshot &base) {
    const PricedSurface *surface = spy_surface(base);
    if (surface == nullptr) {
      return;
    }
    const double spot = surface->pricing().S;
    const std::int64_t ts = surface->pricing().now_ts_ns;
    if (!std::isfinite(spot) || !(spot > 0.0)) {
      return;
    }
    if (!bars_.empty() && bars_.back().ts_ns == ts) {
      return;
    }
    bars_.push_back(OhlcBar{ts, spot, spot, spot, spot});
  }

  IStrategy &inner_;
  TheoEngine engine_;
  std::string symbol_;
  double tenor_years_;
  std::vector<OhlcBar> bars_;
};

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }

  // TheoEdgeSignalStrategy needs a concrete `const PricedSurface&` per step
  // (TheoContext::surface's type, theo.hpp -- unchanged by this driver, no
  // library modification), but this driver's default MarketSnapshot loads are
  // ZERO-COPY VIEW-BACKED (QueryPricingTier::LegacyCompatible, the loader's
  // own default, borrows PricedSurfaceView records rather than reconstructing
  // owned PricedSurface objects -- see MarketSnapshot::load's WS-ZC1 comment,
  // backtest.cpp). `ATX_VOL_ZC_BORROW=0` is that same load path's own
  // documented escape hatch to force the OWNED reconstruct route instead --
  // "it cannot make a run non-deterministic" (backtest.cpp) is the load
  // path's own guarantee that this changes ONLY the surfaces' memory backing,
  // never a priced value, so it cannot be the thing that breaks the NAV
  // byte-identity proof this probe exists to uphold.
  //
  // Gated on `emit_theo_signals` (final-review I3; supersedes fix round 1's
  // gate, which had this backwards): `emit_theo_signals` now defaults to
  // FALSE, so the driver's OWN default (no-flag) runs keep exercising the
  // default zero-copy WS-ZC1 borrow path unchanged. Only an explicit
  // `--theo-signals` run forces the owned-reconstruct route this probe needs
  // -- fix round 1 gated the override on a default-TRUE flag instead, so the
  // driver's default run was silently the one NOT covering borrow, exactly
  // backwards from what its own comment claimed. `_putenv_s` is a Windows CRT
  // API; `MarketSnapshot::load` itself reads this same env var behind a
  // `#if defined(_WIN32)` guard (backtest.cpp) -- guarded here to match
  // rather than assuming the Windows-only toolchain unconditionally.
#if defined(_WIN32)
  if (args.emit_theo_signals) {
    _putenv_s("ATX_VOL_ZC_BORROW", "0");
  }
#endif

  // ── Cross-root manifest + session-timestamp grid ─────────────────────────
  CorpusManifest manifest;
  std::map<std::string, std::int64_t> ts_by_date; // partition key -> session ts
  std::vector<std::string> roots_used;
  for (int year = args.year_lo; year <= args.year_hi; ++year) {
    const std::string root = args.db_prefix + "-" + std::to_string(year);
    if (!fs::exists(root)) continue;
    auto db = SurfaceDb::open(root);
    if (!db) {
      std::fprintf(stderr, "SurfaceDb::open(%s): %s\n", root.c_str(),
                   db.error().to_string().c_str());
      return 1;
    }
    for (const DbPartitionInfo &p : db->partitions()) {
      auto ts = db->session_ts(p.key);
      if (!ts) {
        std::fprintf(stderr, "session_ts(%s @ %s): %s\n", p.key.c_str(), root.c_str(),
                     ts.error().to_string().c_str());
        return 1;
      }
      CorpusEntry entry;
      entry.date = p.key;
      entry.symbol = "SPY";
      entry.status = CorpusFitStatus::Ok;
      entry.archive_path = root + "/partitions/" + p.key + ".atxvsa";
      manifest.entries.push_back(std::move(entry));
      manifest.dates.push_back(p.key);
      ts_by_date[p.key] = *ts;
    }
    roots_used.push_back(root);
  }
  if (manifest.entries.empty()) {
    std::fprintf(stderr, "no partitions found under %s-{%d..%d}\n", args.db_prefix.c_str(),
                 args.year_lo, args.year_hi);
    return 1;
  }
  std::sort(manifest.dates.begin(), manifest.dates.end());
  manifest.dates.erase(std::unique(manifest.dates.begin(), manifest.dates.end()),
                       manifest.dates.end());
  std::sort(manifest.entries.begin(), manifest.entries.end(),
            [](const CorpusEntry &a, const CorpusEntry &b) {
              return a.date != b.date ? a.date < b.date : a.symbol < b.symbol;
            });
  manifest.n_boards = static_cast<std::uint32_t>(manifest.entries.size());
  manifest.n_ok = manifest.n_boards;

  auto full = Clock::from_manifest(manifest);
  if (!full) {
    std::fprintf(stderr, "Clock::from_manifest: %s\n", full.error().to_string().c_str());
    return 1;
  }
  const std::string lo = args.from.empty() ? full->refs().front().date : args.from;
  const std::string hi = args.to.empty() ? full->refs().back().date : args.to;
  auto clock = full->between(lo, hi);
  if (!clock) {
    std::fprintf(stderr, "Clock::between: %s\n", clock.error().to_string().c_str());
    return 1;
  }

  std::vector<std::int64_t> session_ts;
  session_ts.reserve(clock->size());
  for (const SnapshotRef &ref : clock->refs()) session_ts.push_back(ts_by_date.at(ref.date));

  // ── Strategy spec ────────────────────────────────────────────────────────
  StrategySpec spec;
  spec.name = "spy_leaps_40d_strangle_monthly_roll";
  LegSpec leg;
  leg.symbol = "SPY";
  leg.tenor.target_T = args.tenor_years;
  leg.tenor.snap_to_sessions = true;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, args.delta};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, args.delta};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, args.contracts, args.sign};
  spec.legs.push_back(std::move(leg));
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = args.tenor_years - args.roll_months / 12.0;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  spec.session_ts = session_ts;

  DeclarativeStrategy strat(spec);
  RunConfig rc;
  rc.snapshot_cache = std::make_shared<SnapshotCache>();
  // Frictionless by construction: FrictionModel/FinancingConfig defaults are
  // all-zero. NAV is audited against the cash/share/book ledgers every row.
  rc.reconcile_nav = true;
  rc.mark_domain = args.mark_domain == "carry"  ? MarkDomainPolicy::CarryLastMark
                   : args.mark_domain == "error" ? MarkDomainPolicy::Error
                                                 : MarkDomainPolicy::Extrapolate;

  // ── Task 10: read-only theo-edge signal probe (--theo-signals to enable) ──
  IStrategy *active_strategy = &strat;
  std::optional<TheoEdgeSignalStrategy> signal_strat;
  if (args.emit_theo_signals) {
    auto rv_overlay = make_rv_blend_overlay(); // default RvBlendConfig
    if (!rv_overlay) {
      std::fprintf(stderr, "make_rv_blend_overlay: %s\n", rv_overlay.error().to_string().c_str());
      return 1;
    }
    std::vector<std::unique_ptr<ITheoOverlay>> theo_overlays;
    theo_overlays.push_back(std::move(*rv_overlay));
    auto theo_engine = TheoEngine::create(std::move(theo_overlays));
    if (!theo_engine) {
      std::fprintf(stderr, "TheoEngine::create: %s\n", theo_engine.error().to_string().c_str());
      return 1;
    }
    signal_strat.emplace(strat, std::move(*theo_engine), "SPY", args.tenor_years);
    active_strategy = &*signal_strat;
  }

  auto run = run_backtest(*clock, *active_strategy, rc);
  if (!run) {
    std::fprintf(stderr, "run_backtest: %s\n", run.error().to_string().c_str());
    return 1;
  }
  BacktestResult &r = *run;

  const TearSheet ts = tearsheet(r);
  const double final_nav = r.nav.empty() ? 0.0 : r.nav.back();
  const double final_liq = r.nav_liquidation.empty() ? final_nav : r.nav_liquidation.back();

  std::error_code ec;
  fs::create_directories(args.out, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create --out %s: %s\n", args.out.c_str(), ec.message().c_str());
    return 1;
  }

  std::string roots_joined;
  for (const std::string &root : roots_used)
    roots_joined += (roots_joined.empty() ? "" : ";") + root;

  const Meta meta = {
      {"strategy", spec.name},
      {"symbol", "SPY"},
      {"data_source", "surface_db_multi_root"},
      {"db_roots", roots_joined},
      {"requested_from", lo},
      {"requested_to", hi},
      {"window_start", clock->refs().front().date},
      {"window_end", clock->refs().back().date},
      {"n_steps", std::to_string(r.size())},
      {"sessions_in_window", std::to_string(clock->size())},
      {"delta_target", fmt_num(args.delta)},
      {"tenor_years", fmt_num(args.tenor_years)},
      {"roll_months", fmt_num(args.roll_months)},
      {"roll_at_T", fmt_num(spec.lifecycle.roll_at_T)},
      {"contracts", fmt_num(args.contracts)},
      {"sign", fmt_num(args.sign)},
      {"multiplier", "100"},
      {"hedge", "delta_to_zero_daily"},
      {"frictions", "none"},
      {"reconcile_nav", "on"},
      {"theo_signals", args.emit_theo_signals ? "on" : "off"},
      {"final_nav", fmt_num(final_nav)},
      {"final_nav_liquidation", fmt_num(final_liq)},
      {"total_return", fmt_num(ts.total_return)},
      {"ann_vol", fmt_num(ts.ann_vol)},
      {"sharpe", fmt_num(ts.sharpe)},
      {"max_drawdown", fmt_num(ts.max_drawdown)},
      {"hit_rate", fmt_num(ts.hit_rate)},
      {"avg_gross_vega", fmt_num(ts.avg_gross_vega)},
  };

  const std::string track_path = (fs::path(args.out) / "track.tsv").string();
  if (const Status st = write_backtest_pnl_tsv(r, meta, track_path); !st) {
    std::fprintf(stderr, "write_backtest_pnl_tsv: %s\n", st.error().to_string().c_str());
    return 1;
  }
  const std::string series_path = (fs::path(args.out) / "series.tsv").string();
  if (const Status st = write_backtest_tsv(r, series_path); !st) {
    std::fprintf(stderr, "write_backtest_tsv: %s\n", st.error().to_string().c_str());
    return 1;
  }

  std::printf("=== SPY %.0f-delta %.2gy strangle, %.3g-month roll, daily delta hedge ===\n"
              "window %s .. %s | %zu sessions | sign %+.0f x %.0f contract(s)\n"
              "final nav $%.2f (liquidation $%.2f) | sharpe %.2f | maxDD $%.2f\n"
              "wrote %s and %s\n",
              args.delta * 100.0, args.tenor_years, args.roll_months,
              clock->refs().front().date.c_str(), clock->refs().back().date.c_str(),
              clock->size(), args.sign, args.contracts, final_nav, final_liq, ts.sharpe,
              ts.max_drawdown, track_path.c_str(), series_path.c_str());
  return 0;
}
