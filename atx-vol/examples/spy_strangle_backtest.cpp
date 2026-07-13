// atx-vol backtest deliverable — "short the 40-delta 6-month SPY strangle,
// restriked every day", 2026-01-02 -> 2026-07-02.
//
// WHAT: each business day we hold a SHORT strangle (short the ~40-delta OTM call
// + short the ~40-delta OTM put) at the 6-month tenor, and we RESTRIKE it every
// day — close yesterday's strangle and reopen a fresh 40-delta/6m one at today's
// surface. The P&L series is therefore the daily mark-to-market of a rolling short
// 40-delta 6m strangle (frictionless: at mid, the restrike itself is P&L-neutral,
// so the series is pure held-one-day MTM — theta earned vs gamma/vega paid).
//
// HOW (no bespoke strategy code): the DeclarativeStrategy DSL expresses this
// directly. Strangle structure, both legs `{Delta, 0.40}`, `tenor.target_T = 0.5`,
// `size.sign = -1` (short). "Restrike every day" = `Holding::RollAtHorizon` with
// `roll_at_T` set ABOVE the tenor (1.0): the single cohort's residual T (~0.5) is
// always below the threshold, so `lifecycle_decide` clears and reopens it every
// step (strategy.cpp:336). Every recorded row carries exactly 2 open lots.
// These are explicitly MODEL-ON-MODEL continuous contracts, not listed OPRA
// instruments or executable fills. Use `spy_strangle_tradeable` for the existing
// listed OPRA contract/quote workflow.
//
// DATA (no paid Databento pull; synthetic per the house rule): a deterministic
// rolling-vol synthetic SPY corpus. A seeded (never time-based) mt19937_64 drives
// an evolving spot path (~12%/yr realized) and a mean-reverting ATM-vol regime; a
// per-date eSSVI `PricedSurface` (analytic, no fit) is written to a per-date ATXVSA
// v3 archive. The surface's [0.05,1.0] slice grid spans the 6m tenor, so the
// 40-delta strike solve and the T=0.5 reprice are exercised exactly as in
// production. Timestamps are real calendar dates, so aging (theta) sees the true
// day count (incl. weekend 3-day gaps).
//
// STORAGE this driver exercises + confirms:
//   * surface binary archive = ATXVSA v3 (write_surface_archive_file / SurfaceArchive):
//     O(1) symbol lookup, CRC-32C integrity. The corpus IS this store; we report
//     bytes/surface and single-symbol reload latency.
//   * OPRA option quote slices = QuoteFrame (make_synthetic_american_panel), the
//     in-memory OPRA slice, persisted via the committed self-contained CSV store
//     (load_chain_csv). We write one representative slice, reload it, and confirm
//     the round-trip. (The Parquet loader is deferred/NotImplemented by house rule.)
//
// TIMING: the backtest run (build-corpus excluded) is wall-timed; we report
// steps/s, leg-reprices/s, and the per-contract reprice cost.
//
// OFF by default (ATX_BUILD_EXAMPLES); the correctness GATE is the self-contained
// tests/spy_strangle_backtest_test.cpp.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp" // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp" // Clock, run_backtest, RunConfig, BacktestResult, MarketSnapshot
#include "atx/vol/corpus.hpp"   // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/data.hpp"     // iso_to_ns, ns_to_iso_date, year_fraction, QuoteFrame
#include "atx/vol/panel.hpp"    // make_synthetic_american_panel, SynthPanelSpec, load_chain_csv
#include "atx/vol/priced_surface.hpp" // PricedSurface, PricingContext
#include "atx/vol/s3.hpp"             // S3Params
#include "atx/vol/strategy.hpp"       // DeclarativeStrategy, StrategySpec
#include "atx/vol/surface_archive.hpp" // write_surface_archive_file, SurfaceArchiveItem, SurfaceArchive
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/tearsheet.hpp"      // TearSheet, tearsheet, write_backtest_tsv
#include "atx/vol/types.hpp"          // Side, Result, Status
#include "atx/vol/vol_curve.hpp"      // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"    // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kSpyUid = 42;
constexpr double kTenorT = 0.5; // 6-month strangle

// Business days (Mon-Fri; market holidays ignored — a synthetic calendar) in
// [start_iso, end_iso] inclusive. Epoch day 0 (1970-01-01) is a Thursday, so a
// UTC-midnight ns maps to weekday `((day % 7) + 4) % 7` with 0=Sun..6=Sat.
[[nodiscard]] std::vector<std::string> business_days(const std::string &start_iso,
                                                     const std::string &end_iso) {
  std::vector<std::string> out;
  const std::int64_t s = iso_to_ns(start_iso);
  const std::int64_t e = iso_to_ns(end_iso);
  for (std::int64_t ns = s; ns <= e; ns += kDayNs) {
    const long long day = ns / kDayNs;
    const int dow = static_cast<int>(((day % 7) + 4) % 7);
    if (dow == 0 || dow == 6) {
      continue; // weekend
    }
    out.push_back(ns_to_iso_date(ns));
  }
  return out;
}

// A synthetic eSSVI SPY PricedSurface: index term structure (ATM vol rising with
// tenor), a `vol_bump` regime shift applied uniformly across slices, genuine
// American premium via a 0.02 effective carry. Slice grid [0.05,1.0] spans the 6m
// tenor. Mirrors the backtest_bench / tearsheet make_surface pattern.
[[nodiscard]] PricedSurface make_spy_surface(double S, std::int64_t now_ts, double vol_bump) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump; // ATM total variance level
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i); // index crash-put skew
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = S; // flat forward = spot (the fixture/bench convention)
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
  pc.uid = kSpyUid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  if (!ps) {
    std::fprintf(stderr, "make_spy_surface: %s\n", ps.error().to_string().c_str());
    std::exit(1);
  }
  return std::move(*ps);
}

// Write one date's SPY archive; return its path.
[[nodiscard]] std::string write_archive(const fs::path &dir, const std::string &date,
                                        const PricedSurface &spy) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const SurfaceArchiveItem item{"SPY", &spy};
  const std::span<const SurfaceArchiveItem> items(&item, 1);
  const Status st = write_surface_archive_file(path, items);
  if (!st) {
    std::fprintf(stderr, "write_archive: %s\n", st.error().to_string().c_str());
    std::exit(1);
  }
  return path;
}

[[nodiscard]] CorpusManifest
make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths) {
  CorpusManifest m;
  for (const auto &[date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = "SPY";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

// The short 40-delta 6m strangle, restriked every day, sized by `size`.
[[nodiscard]] StrategySpec make_strangle_spec(SizeSpec size) {
  StrategySpec spec;
  spec.name = "spy-short-40d-6m-strangle-daily-restrike";
  LegSpec leg;
  leg.symbol = "SPY"; // resolved to uid per snapshot (works for synthetic + real corpora)
  leg.tenor.target_T = kTenorT;
  leg.tenor.snap_to_listed = false; // model-on-model; listed route is a separate workflow
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.size = size;
  spec.legs.push_back(leg);
  // Restrike every day: a single cohort, rolled whenever residual T < roll_at_T.
  // With roll_at_T = 1.0 > the 0.5 tenor, the ~0.5 residual is always below it, so
  // the book is cleared + reopened at fresh 40-delta/6m strikes every step.
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = 1.0;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::None, HedgeSpec::Cadence::Daily, 0.0};
  return spec;
}

// ── Storage confirmation: one OPRA quote slice -> CSV -> reload ──────────────

// Persist a QuoteFrame as the documented self-contained CSV chain (panel.hpp
// load_chain_csv format): uid,snapshot_iso,spot,expiry_iso,strike,side,bid,ask,
// bid_size,ask_size,under_spot. Returns the byte size written.
[[nodiscard]] std::uintmax_t write_chain_csv(const fs::path &path, const QuoteFrame &f) {
  std::ofstream os(path, std::ios::binary | std::ios::trunc);
  os << "uid,snapshot_iso,spot,expiry_iso,strike,side,bid,ask,bid_size,ask_size,under_spot\n";
  for (const QuoteRow &r : f.rows) {
    const std::string uid = r.uid.empty() ? f.uid : r.uid;
    os << uid << ',' << f.snapshot_iso << ',' << f.spot << ',' << r.expiry_iso << ',' << r.strike
       << ',' << (r.side == Side::Call ? 'C' : 'P') << ',' << r.bid << ',' << r.ask << ','
       << r.bid_size << ',' << r.ask_size << ',' << f.spot << '\n';
  }
  os.flush();
  std::error_code ec;
  return fs::file_size(path, ec);
}

// Build one dense SPY-like OPRA quote slice with rolling expiries and round-trip
// it through the CSV store. Prints a confirmation line. Independent of the corpus.
void confirm_quote_slice_store(const fs::path &dir) {
  const std::string snap = "2026-04-01";
  SynthPanelSpec spec;
  spec.uid = "SPY";
  spec.snapshot_iso = snap;
  spec.spot = 600.0;
  spec.r = kR;
  spec.borrow = 0.0;
  // Rolling expiries relative to the snapshot (1m,2m,3m,6m,9m) so the 6m tenor is
  // present; ns_to_iso_date turns each offset into a civil date.
  const int offs[] = {30, 60, 91, 182, 273};
  const std::int64_t snap_ns = iso_to_ns(snap);
  for (int off : offs) {
    SynthExpiry e;
    e.expiry_iso = ns_to_iso_date(snap_ns + static_cast<std::int64_t>(off) * kDayNs);
    e.T = year_fraction(snap, e.expiry_iso);
    const double s2 = 2.0 * std::sqrt(e.T) * -0.55; // index strike-skew
    e.truth = S3Params{0.10 + 0.05 * e.T, s2, 0.4};
    spec.expiries.push_back(e);
  }
  for (double K = 540.0; K <= 660.0 + 1e-9; K += 5.0) {
    spec.strikes.push_back(K);
  }
  spec.half_spread_frac = 0.006;
  spec.min_half_spread = 0.01;

  const Result<SynthPanel> panel = make_synthetic_american_panel(spec);
  if (!panel) {
    std::fprintf(stderr, "confirm_quote_slice_store: %s\n", panel.error().to_string().c_str());
    return;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path csv = dir / (snap + "_SPY_chain.csv");
  const std::uintmax_t bytes = write_chain_csv(csv, panel->frame);

  CsvChainSpec cs;
  cs.path = csv.string();
  cs.yc_pillar_t = {0.25, 1.0};
  cs.yc_pillar_r = {kR, kR};
  const Result<QuoteFrame> reloaded = load_chain_csv(cs);
  const bool ok = reloaded.has_value() && reloaded->rows.size() == panel->frame.rows.size();
  std::printf(
      "[storage] OPRA quote slice: %zu quotes -> CSV %.1f KB -> reload %s (%zu rows round-trip)\n",
      panel->frame.rows.size(), static_cast<double>(bytes) / 1024.0, ok ? "OK" : "MISMATCH",
      reloaded.has_value() ? reloaded->rows.size() : 0u);
}

} // namespace

int main(int argc, char **argv) {
  // Data source: a REAL corpus manifest (e.g. from spy_ytd_corpus over a Databento
  // OPRA pull) when --manifest / a positional path is given, else the built-in
  // deterministic synthetic corpus. --theta-per-day sets the constant-risk target.
  std::string manifest_path;
  double target_theta_per_day = 10'000.0; // $/day book theta (constant-risk sizing)
  QueryPricingTier query_pricing_tier = QueryPricingTier::LegacyCompatible;
  std::string_view query_pricing_label = "legacy";
  bool preload_snapshots = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--manifest") {
      manifest_path = nv();
    } else if (a == "--theta-per-day") {
      target_theta_per_day = std::strtod(nv(), nullptr);
    } else if (a == "--query-tier") {
      const std::string_view value = nv();
      if (value == "legacy") {
        query_pricing_tier = QueryPricingTier::LegacyCompatible;
      } else if (value == "cold") {
        query_pricing_tier = QueryPricingTier::ColdReference;
      } else if (value == "representative") {
        query_pricing_tier = QueryPricingTier::RepresentativeFast;
      } else if (value == "carry") {
        query_pricing_tier = QueryPricingTier::CarryBank;
      } else {
        std::fprintf(stderr,
                     "invalid --query-tier '%.*s' (expected legacy, cold, representative, or "
                     "carry)\n",
                     static_cast<int>(value.size()), value.data());
        return 2;
      }
      query_pricing_label = value;
    } else if (a == "--preload-snapshots") {
      preload_snapshots = true;
    } else if (!a.empty() && a.front() != '-' && manifest_path.empty()) {
      manifest_path = argv[i]; // positional manifest path
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 2;
    }
  }
  const bool synthetic = manifest_path.empty();

  const fs::path base = fs::temp_directory_path() / "atx-spy-strangle-backtest";
  std::error_code ec;
  const fs::path arch_dir = base / "archives";

  // ── 1. Corpus: synthetic (built here) OR real (loaded from a manifest) ─────
  std::vector<std::string> dates;
  std::string first_archive; // representative archive path (reload-latency probe)
  std::string data_source;
  double build_ms = 0.0;
  std::uintmax_t total_arch_bytes = 0;
  std::optional<Clock> clock;

  if (synthetic) {
    fs::remove_all(base, ec);
    data_source = "synthetic rolling-vol eSSVI SPY corpus";
    dates = business_days("2026-01-02", "2026-07-02");
    std::mt19937_64 rng(0x5391A11ED5EEDULL); // fixed seed; NEVER time-based
    std::normal_distribution<double> z(0.0, 1.0);
    const double sig_d = 0.12 / std::sqrt(252.0);           // ~12%/yr realized daily vol
    const double mu_d = 0.05 / 252.0 - 0.5 * sig_d * sig_d; // small drift, Ito-corrected
    double S = 600.0;
    double vb = 0.0; // AR(1) vol-regime bump
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, std::string>> dp;
    dp.reserve(dates.size());
    for (const std::string &date : dates) {
      const std::int64_t now = iso_to_ns(date);
      const PricedSurface spy = make_spy_surface(S, now, vb);
      const std::string path = write_archive(arch_dir, date, spy);
      total_arch_bytes += fs::file_size(path, ec);
      dp.emplace_back(date, path);
      // Advance the seeded path: a slow AR(1) vol regime with a realistic vol-of-vol.
      S *= std::exp(mu_d + sig_d * z(rng));
      vb = 0.96 * vb + 0.004 * z(rng);
      vb = std::min(0.05, std::max(-0.015, vb)); // keep eSSVI theta > 0
    }
    build_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    first_archive = dp.front().second;
    auto ck = Clock::from_manifest(make_manifest(dp));
    if (!ck) {
      std::fprintf(stderr, "clock: %s\n", ck.error().to_string().c_str());
      return 1;
    }
    clock = std::move(*ck);
  } else {
    data_source = "REAL OPRA (Databento cbbo-1m, 19:55Z NBBO)";
    const Result<CorpusManifest> man = read_manifest_file(manifest_path);
    if (!man) {
      std::fprintf(stderr, "read_manifest_file(%s): %s\n", manifest_path.c_str(),
                   man.error().to_string().c_str());
      return 1;
    }
    for (const CorpusEntry &e : man->entries) {
      if (e.status != CorpusFitStatus::Ok) {
        continue;
      }
      if (dates.empty() || dates.back() != e.date) {
        dates.push_back(e.date);
      }
      if (first_archive.empty()) {
        first_archive = e.archive_path;
      }
      std::error_code fe;
      total_arch_bytes += fs::file_size(e.archive_path, fe);
    }
    auto ck = Clock::from_manifest(*man);
    if (!ck) {
      std::fprintf(stderr, "clock: %s\n", ck.error().to_string().c_str());
      return 1;
    }
    clock = std::move(*ck);
  }
  if (dates.empty()) {
    std::fprintf(stderr, "corpus has no dates\n");
    return 1;
  }
  // A real-manifest run does not build the synthetic corpus above, so it has
  // not created `base` as a side effect. Create the report directory explicitly
  // before write_backtest_tsv / the CSV writer use it on a fresh machine.
  fs::create_directories(base, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create output directory %s: %s\n", base.string().c_str(),
                 ec.message().c_str());
    return 1;
  }
  std::printf("[data] %s: %zu dates %s..%s\n", data_source.c_str(), dates.size(),
              dates.front().c_str(), dates.back().c_str());

  // ── 2. Run the backtest (timed) ───────────────────────────────────────────
  // Constant-risk sizing: resolve the unit count DAILY so the book theta is pinned
  // at `target_theta_per_day` $/DAY regardless of where the surface is. `sign=-1`
  // => short; under the daily restrike the per-unit theta moves with spot/vol, so
  // the resolved unit count floats each day to hold it. (gross_theta is annualized
  // == target * 365.25.)
  const double kTargetThetaPerDay = target_theta_per_day;
  const SizeSpec size{SizeSpec::Kind::TargetTheta, kTargetThetaPerDay, -1.0};
  const StrategySpec spec = make_strangle_spec(size);
  DeclarativeStrategy strat{spec};
  RunConfig run_config;
  run_config.query_pricing_tier = query_pricing_tier;
  double preload_ms = 0.0;
  if (preload_snapshots) {
    run_config.snapshot_cache = std::make_shared<SnapshotCache>();
    run_config.prefetch_snapshots = false;
    const auto preload_start = std::chrono::steady_clock::now();
    for (const SnapshotRef &ref : clock->refs()) {
      auto snapshot = run_config.snapshot_cache->load(ref.archive_path, query_pricing_tier);
      if (!snapshot.has_value()) {
        std::fprintf(stderr, "preload(%s): %s\n", ref.archive_path.c_str(),
                     snapshot.error().to_string().c_str());
        return 1;
      }
    }
    preload_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - preload_start)
            .count();
    std::printf("[prep] preloaded %zu snapshots for %.*s tier in %.1f ms\n", clock->size(),
                static_cast<int>(query_pricing_label.size()), query_pricing_label.data(),
                preload_ms);
  }
  const auto t_run0 = std::chrono::steady_clock::now();
  auto res = run_backtest(*clock, strat, run_config);
  const auto t_run1 = std::chrono::steady_clock::now();
  if (!res) {
    std::fprintf(stderr, "run_backtest: %s\n", res.error().to_string().c_str());
    return 1;
  }
  const BacktestResult &r = *res;
  const TearSheet t = tearsheet(r);

  // ── 3. Table + tearsheet ──────────────────────────────────────────────────
  const std::string tsv = (base / "spy_short_strangle.tsv").string();
  const Status st = write_backtest_tsv(r, tsv);
  if (!st) {
    std::fprintf(stderr, "tsv: %s\n", st.error().to_string().c_str());
    return 1;
  }

  const double run_ms = std::chrono::duration<double, std::milli>(t_run1 - t_run0).count();
  const int priced_steps = static_cast<int>(r.size()) - 1;

  // CSV (pnl + greeks) with a `# key=value` metadata header the Python tearsheet
  // reads. Values written at full round-trip precision.
  const std::string csv = (base / "spy_short_strangle.csv").string();
  {
    std::ofstream os(csv, std::ios::binary | std::ios::trunc);
    const double run_s0 = run_ms / 1000.0;
    const double sps = (run_s0 > 0.0) ? static_cast<double>(priced_steps) / run_s0 : 0.0;
    os.setf(std::ios::fmtflags(0), std::ios::floatfield);
    os.precision(10);
    os << "# symbol=SPY\n"
       << "# strategy=Short 40-Delta 6M Strangle, Daily Restrike\n"
       << "# contract_semantics=model-on-model continuous surface contracts (not listed fills)\n"
       << "# listed_workflow=spy_strangle_tradeable via listed_opra.hpp\n"
       << "# data_source=" << data_source << "\n"
       << "# query_pricing_tier=" << query_pricing_label << "\n"
       << "# snapshot_preload_ms=" << preload_ms << "\n"
       << "# window_start=" << dates.front() << "\n"
       << "# window_end=" << dates.back() << "\n"
       << "# business_days=" << dates.size() << "\n"
       << "# priced_steps=" << priced_steps << "\n"
       << "# multiplier=100\n"
       << "# tenor_years=" << kTenorT << "\n"
       << "# delta_target=0.40\n"
       << "# sizing=Target book theta $" << kTargetThetaPerDay << "/day (units resolved daily)\n"
       << "# target_theta_daily=" << kTargetThetaPerDay << "\n"
       << "# target_theta=" << (kTargetThetaPerDay * 365.25) << "\n" // annual; matches gross_theta
       << "# wall_clock_ms=" << run_ms << "\n"
       << "# steps_per_s=" << sps << "\n"
       << "# total_return=" << t.total_return << "\n"
       << "# ann_return=" << t.ann_return << "\n"
       << "# ann_vol=" << t.ann_vol << "\n"
       << "# sharpe=" << t.sharpe << "\n"
       << "# max_drawdown=" << t.max_drawdown << "\n"
       << "# hit_rate=" << t.hit_rate << "\n"
       << "# avg_gross_vega=" << t.avg_gross_vega << "\n"
       << "date,pnl_total,nav,pnl_theta,pnl_vega,pnl_gamma,pnl_delta,pnl_vanna,pnl_volga,"
          "pnl_rho,pnl_charm,pnl_unexplained,gross_delta,gross_gamma,gross_vega,gross_theta,"
          "n_open_lots\n";
    for (std::size_t i = 0; i < r.size(); ++i) {
      os << r.date[i] << ',' << r.pnl_total[i] << ',' << r.nav[i] << ',' << r.pnl_theta[i] << ','
         << r.pnl_vega[i] << ',' << r.pnl_gamma[i] << ',' << r.pnl_delta[i] << ',' << r.pnl_vanna[i]
         << ',' << r.pnl_volga[i] << ',' << r.pnl_rho[i] << ',' << r.pnl_charm[i] << ','
         << r.pnl_unexplained[i] << ',' << r.gross_delta[i] << ',' << r.gross_gamma[i] << ','
         << r.gross_vega[i] << ',' << r.gross_theta[i] << ',' << r.n_open_lots[i] << '\n';
    }
  }
  const long long leg_reprices =
      static_cast<long long>(priced_steps) * 2; // 2 lots repriced per step
  const double run_s = run_ms / 1000.0;
  const double steps_per_s = (run_s > 0.0) ? static_cast<double>(priced_steps) / run_s : 0.0;

  std::printf("\n=== SPY short 40-delta 6m strangle, restriked daily (%s -> %s) ===\n",
              dates.front().c_str(), dates.back().c_str());
  std::printf("contract semantics: model-on-model continuous surface contracts (not listed fills)\n"
              "listed OPRA workflow: spy_strangle_tradeable via listed_opra.hpp\n");
  std::printf("corpus: %s | %zu dates, %.1f KB archives (%.2f KB/surface)%s\n", data_source.c_str(),
              dates.size(), static_cast<double>(total_arch_bytes) / 1024.0,
              static_cast<double>(total_arch_bytes) / 1024.0 / static_cast<double>(dates.size()),
              synthetic ? "" : " [fit offline by spy_ytd_corpus]");
  if (synthetic) {
    std::printf("synthetic-corpus build: %.0f ms\n", build_ms);
    confirm_quote_slice_store(base / "quotes");
  }

  // Archive reload latency (single-symbol map).
  {
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kReps = 200;
    volatile std::size_t sink = 0;
    for (int i = 0; i < kReps; ++i) {
      auto snap = MarketSnapshot::load(first_archive);
      if (snap) {
        sink += snap->surfaces().size();
      }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kReps;
    std::printf("[storage] surface archive reload: %.1f us/open (single SPY surface)\n", us);
    (void)sink;
  }

  std::printf("\n[timing] backtest run (%.*s query tier): %.1f ms over %d priced steps => %.1f "
              "steps/s, "
              "%lld leg-reprices (%.3f ms/step, %.3f ms/leg-reprice)\n",
              static_cast<int>(query_pricing_label.size()), query_pricing_label.data(), run_ms,
              priced_steps, steps_per_s, leg_reprices,
              (priced_steps > 0) ? run_ms / priced_steps : 0.0,
              (leg_reprices > 0) ? run_ms / static_cast<double>(leg_reprices) : 0.0);

  std::printf("\n[tearsheet] (short 40d 6m strangle, TARGET book theta $%.0f/day, mult 100, "
              "frictionless)\n",
              kTargetThetaPerDay);
  std::printf("  total_return   = %.2f  ($ PnL, cumulative)\n", t.total_return);
  std::printf("  ann_return     = %.2f   ann_vol = %.2f   sharpe = %.3f\n", t.ann_return, t.ann_vol,
              t.sharpe);
  std::printf("  max_drawdown   = %.2f   hit_rate = %.3f\n", t.max_drawdown, t.hit_rate);
  std::printf("  avg_gross_vega = %.2f   avg_gross_gamma = %.4f\n", t.avg_gross_vega,
              t.avg_gross_gamma);
  std::printf("  attribution: theta=%.2f vega=%.2f delta=%.2f gamma=%.2f vanna=%.2f "
              "volga=%.2f rho=%.2f charm=%.2f unexpl=%.2f\n",
              t.attr_theta, t.attr_vega, t.attr_delta, t.attr_gamma, t.attr_vanna, t.attr_volga,
              t.attr_rho, t.attr_charm, t.attr_unexplained);

  // A compact head/tail table of the daily series.
  std::printf("\n  %-12s %10s %12s %12s %12s %12s\n", "date", "pnl_total", "nav", "gross_vega",
              "gross_theta", "n_lots");
  const auto row = [&](std::size_t i) {
    std::printf("  %-12s %10.2f %12.2f %12.2f %12.2f %12.0f\n", r.date[i].c_str(), r.pnl_total[i],
                r.nav[i], r.gross_vega[i], r.gross_theta[i], r.n_open_lots[i]);
  };
  for (std::size_t i = 0; i < r.size() && i < 5; ++i) {
    row(i);
  }
  if (r.size() > 10) {
    std::printf("  %-12s %10s %12s %12s %12s %12s\n", "...", "...", "...", "...", "...", "...");
    for (std::size_t i = r.size() - 5; i < r.size(); ++i) {
      row(i);
    }
  }
  std::printf("\n[wrote] %s (%zu rows)\n", tsv.c_str(), r.size());
  std::printf("[wrote] %s (pnl + greeks + metadata header for the Python tearsheet)\n",
              csv.c_str());

  return 0;
}
