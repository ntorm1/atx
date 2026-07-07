// atx-vol backtest engine (Phase C0) — throughput benchmark (smoke).
//
// Establishes a deterministic steps/s number for `run_backtest` over a non-trivial
// multi-underlier book, and guards it with a GENEROUS ceiling (>= 5x headroom, a
// smoke guard — NOT a tight perf gate, so it never flakes).
//
// SYNTHETIC surfaces only (the backtest_test / tearsheet_test make_surface pattern
// — analytic eSSVI, no fitting): a real corpus fit is slow and would swamp the
// timing. We write one synthetic archive per date (U surfaces, distinct uids /
// symbols) over D dates, then run a multi-leg straddle strategy (one straddle clip
// per underlier, EveryStep / HoldToExpiry) so the book is genuinely large. The run
// is timed with std::chrono::steady_clock (the same mechanic as
// Corpus.Throughput_FitsUnderCeiling). Determinism is NOT re-checked here (the real
// litmus covers it) — this stays a pure timing smoke.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"         // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"         // Clock, run_backtest, RunConfig, BacktestResult
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"         // DeclarativeStrategy, StrategySpec
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/types.hpp"            // Side, Result, Status
#include "atx/vol/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;

// Bench dimensions. D dates x U underliers. A straddle clip (2 legs per underlier)
// is opened EVERY step and held to expiry, so the book grows to D*U*2 lots — a
// genuinely non-trivial multi-underlier American book. Sized so the whole run
// stays a few seconds (American greeks over a growing book are ~ms/lot): the real
// litmus (backtest_real_test) already proves the full engine, so this is a pure
// timing smoke. The straddle tenor stays inside the synthetic grid's [0.05, 1.0] T
// span across the whole run (front cohort ages to ~0.10, never below 0.05).
constexpr int kD = 20;                  // dates (=> D-1 priced steps)
constexpr int kU = 4;                   // underliers per date
constexpr std::uint32_t kUidBase = 100;
constexpr double kTargetT = 0.15;       // straddle tenor (in-grid all run)

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors backtest_test's make_surface.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd,
                                         std::int64_t now_ts, double vol_bump = 0.0) {
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
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

// Write `items` (symbol -> surface) as one date's archive; return its path.
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
  const Status st = write_surface_archive_file(path, its);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows.
[[nodiscard]] CorpusManifest make_manifest(
    const std::vector<std::pair<std::string, std::string>>& date_paths) {
  CorpusManifest m;
  for (const auto& [date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = "U0";  // first-Ok archive per date is all the clock needs
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

}  // namespace

// ── Throughput smoke: D dates x U underliers, straddle clips held to expiry ──
TEST(BacktestBench, MultiUnderlierStraddle_StepsPerSecond) {
  const fs::path dir = fs::temp_directory_path() / "atx-backtest-bench";
  std::error_code ec;
  fs::remove_all(dir, ec);

  // Build D per-date archives, each holding U distinct-uid synthetic surfaces.
  // Surfaces are kept alive in `owned` for the duration of every archive write.
  std::vector<std::pair<std::string, std::string>> dp;
  dp.reserve(static_cast<std::size_t>(kD));
  for (int d = 0; d < kD; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> owned;
    owned.reserve(static_cast<std::size_t>(kU));
    std::vector<std::pair<std::string, const PricedSurface*>> items;
    items.reserve(static_cast<std::size_t>(kU));
    for (int u = 0; u < kU; ++u) {
      const double S = (100.0 + 10.0 * static_cast<double>(u)) *
                       (1.0 + 0.003 * static_cast<double>(d));
      const double vb = 0.001 * static_cast<double>(d) + 0.002 * static_cast<double>(u);
      owned.push_back(make_surface(kUidBase + static_cast<std::uint32_t>(u), S, S, now, vb));
    }
    std::vector<std::string> syms;
    syms.reserve(static_cast<std::size_t>(kU));
    for (int u = 0; u < kU; ++u) {
      syms.push_back("U" + std::to_string(u));
      items.emplace_back(syms.back(), &owned[static_cast<std::size_t>(u)]);
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "2027-%02d-%02d", 1 + d / 28, 1 + d % 28);
    dp.emplace_back(buf, write_archive(dir, buf, items));
  }
  auto clock = Clock::from_manifest(make_manifest(dp));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ASSERT_EQ(clock->size(), static_cast<std::size_t>(kD));

  // One ATM straddle clip per underlier, a fresh cohort every step, held to expiry.
  StrategySpec spec;
  spec.name = "bench-multi-underlier-straddle";
  for (int u = 0; u < kU; ++u) {
    LegSpec leg;
    leg.uid = kUidBase + static_cast<std::uint32_t>(u);
    leg.tenor.target_T = kTargetT;
    leg.structure.kind = StructureSpec::Kind::Straddle;  // ATM call + put
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(leg);
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;

  DeclarativeStrategy strat{spec};

  const auto t0 = std::chrono::steady_clock::now();
  auto res = run_backtest(*clock, strat);
  const auto t1 = std::chrono::steady_clock::now();
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;
  ASSERT_EQ(r.size(), static_cast<std::size_t>(kD));

  const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double wall_s = wall_ms / 1000.0;
  const int priced_steps = kD - 1;                 // headline unit
  const long long leg_steps = static_cast<long long>(priced_steps) * kU * 2;  // straddle = 2 legs
  const double steps_per_s = (wall_s > 0.0) ? static_cast<double>(priced_steps) / wall_s : 0.0;
  const double leg_steps_per_s = (wall_s > 0.0) ? static_cast<double>(leg_steps) / wall_s : 0.0;

  // Book grew genuinely non-trivial (many overlapping straddle cohorts).
  EXPECT_GT(r.n_open_lots.back(), static_cast<double>(kU));

  // Generous smoke ceiling (nominal wall ~5 s => ~12x headroom, matching the
  // Corpus.Throughput convention): a pure timing guard, not a perf gate.
  EXPECT_LT(wall_ms, 60000.0) << "throughput ceiling exceeded";

  std::printf(
      "[backtest-bench] D=%d U=%d priced_steps=%d leg_steps=%lld wall=%.0f ms "
      "steps/s=%.1f leg_steps/s=%.1f final_open_lots=%.0f\n",
      kD, kU, priced_steps, leg_steps, wall_ms, steps_per_s, leg_steps_per_s,
      r.n_open_lots.back());

  fs::remove_all(dir, ec);
}
