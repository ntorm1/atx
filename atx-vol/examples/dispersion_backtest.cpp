// atx-vol backtest engine — a dispersion worked example (Phase B3).
//
// Builds a small synthetic eSSVI corpus (an index + two constituents, no
// external data), runs the `DispersionStrategy` (the P4-1 vega-neutral book with
// an implied-correlation signal) through `run_backtest`, folds the result into a
// `TearSheet`, prints headline metrics + the implied-correlation signal, and
// writes the full series (including the signal column) to a TSV via
// `write_backtest_tsv`. OFF by default (ATX_BUILD_EXAMPLES).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp" // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"        // Clock
#include "atx/vol/backtest_driver.hpp" // run_timed (the timed engine + tearsheet + stats spine)
#include "atx/vol/corpus.hpp"          // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/detail/counters.hpp"
#include "atx/vol/dispersion.hpp" // DispersionUniverse, DispersionConfig, DispersionMember
#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/detail/phase_profile.hpp"
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"        // DispersionStrategy
#include "atx/vol/surface_archive.hpp" // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/tools/tearsheet.hpp"       // tearsheet, write_backtest_tsv
#include "atx/vol/types.hpp"           // Side, Result, Status
#include "atx/vol/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"     // EssviParams

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

[[nodiscard]] std::string
write_archive(const fs::path &dir, const std::string &date,
              const std::vector<std::pair<std::string, const PricedSurface *>> &items) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  std::vector<SurfaceArchiveItem> its;
  its.reserve(items.size());
  for (const auto &[sym, ps] : items) {
    its.push_back(SurfaceArchiveItem{sym, ps});
  }
  const Status st = write_surface_archive_v2_file(path, its);
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
    e.symbol = "MKT";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

} // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "atx-dispersion-backtest";
  std::error_code ec;
  fs::remove_all(dir, ec);

  const std::vector<int> day_off = {0, 5, 10, 15, 20};
  std::vector<std::pair<std::string, std::string>> dp;
  for (std::size_t d = 0; d < day_off.size(); ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(day_off[d]) * kDayNs;
    const double drift = 1.0 + 0.001 * static_cast<double>(day_off[d]);
    const PricedSurface idx = make_surface(1, 500.0 * drift, 500.0 * drift, now, 0.00);
    const PricedSurface n0 = make_surface(2, 100.0 * drift, 100.0 * drift, now, 0.02);
    const PricedSurface n1 = make_surface(3, 120.0 * drift, 120.0 * drift, now, 0.03);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-10-%02d", static_cast<int>(d + 1));
    dp.emplace_back(std::string(buf),
                    write_archive(dir, buf, {{"IDX", &idx}, {"NM0", &n0}, {"NM1", &n1}}));
  }
  auto clock = Clock::from_manifest(make_manifest(dp));
  if (!clock) {
    std::fprintf(stderr, "clock: %s\n", clock.error().to_string().c_str());
    return 1;
  }

  DispersionUniverse u;
  u.index = DispersionMember{"IDX", 1, 0.0};
  u.names.push_back(DispersionMember{"NM0", 2, 0.6});
  u.names.push_back(DispersionMember{"NM1", 3, 0.4});
  DispersionBacktestConfig config;
  config.min_names = 2u;
  config.record_diagnostics = true; // worked example opts into research output
#if defined(ATX_VOL_PROFILE)
  phase_profile::reset();
#endif
#if defined(ATX_VOL_COUNTERS)
  counters::reset();
#endif
  // Stages 5+6+7 via the `backtest_driver` spine; it times the engine call ONLY,
  // so `engine_ms` keeps exactly its pre-migration meaning.
  auto outcome = run_timed(*clock, u, config);
  if (!outcome) {
    std::fprintf(stderr, "run: %s\n", outcome.error().to_string().c_str());
    return 1;
  }
  const BacktestResult &r = outcome->result;
  const TearSheet t = outcome->sheet;
  const double engine_ms = outcome->stats.wall_clock_ms;

  std::printf("[dispersion] rows=%zu engine_ms=%.3f total_return=%.4f sharpe=%.4f max_dd=%.4f "
              "avg_gross_vega=%.2f pnl_per_vega_traded=%.6f\n",
              r.size(), engine_ms, t.total_return, t.sharpe, t.max_drawdown, t.avg_gross_vega,
              t.pnl_per_vega_traded);
  for (const auto &sig : r.signals) {
    if (!sig.second.empty()) {
      std::printf("[dispersion] signal '%s': first=%.6f last=%.6f\n", sig.first.c_str(),
                  sig.second.front(), sig.second.back());
    }
  }
#if defined(ATX_VOL_PROFILE)
  {
    const phase_profile::Snapshot profile = phase_profile::snapshot();
    for (unsigned i = 0; i < phase_profile::kCount; ++i) {
      if (profile.calls[i] != 0u) {
        std::printf("[profile] %s calls=%llu total_ms=%.3f\n", phase_profile::kNames[i],
                    static_cast<unsigned long long>(profile.calls[i]),
                    static_cast<double>(profile.nanoseconds[i]) / 1.0e6);
      }
    }
  }
#endif
#if defined(ATX_VOL_COUNTERS)
  {
    const counters::Snapshot measured = counters::snapshot();
    for (unsigned i = 0; i < counters::kCount; ++i) {
      if (measured.values[i] != 0u) {
        std::printf("[counter] %s=%llu\n", counters::kNames[i],
                    static_cast<unsigned long long>(measured.values[i]));
      }
    }
  }
#endif

  const std::string tsv = (dir / "dispersion.tsv").string();
  const Status st = write_backtest_tsv(r, tsv);
  if (!st) {
    std::fprintf(stderr, "tsv: %s\n", st.error().to_string().c_str());
    return 1;
  }
  std::printf("[dispersion] wrote %s\n", tsv.c_str());
  return 0;
}
