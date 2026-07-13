// fit_timing_probe.cpp — TEMPORARY perf probe (NOT for commit).
// Mirrors fit_curve_surface's per-chain loop with per-phase wall-clock timers to
// locate where a full-board ConvexDense fit spends its ~100s. Loads the exact
// cached SPY parquet the slow tests use.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>

#include "atx/vol/arb.hpp"
#include "atx/vol/calib.hpp"
#include "atx/vol/curve_fit.hpp"
#include "atx/vol/deamer.hpp"
#include "atx/vol/opra_panel.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_curve.hpp"

using namespace atx::vol;
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a, clk::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

int main() {
  const char* cands[] = {
      "data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "C:/atx/data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
  };
  std::string path;
  for (const char* c : cands) {
    if (std::filesystem::exists(c)) {
      path = c;
      break;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr, "parquet not found\n");
    return 1;
  }

  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  const auto panel = load_opra_cbbo_parquet(spec);
  if (!panel) {
    std::fprintf(stderr, "load: %s\n", panel.error().to_string().c_str());
    return 1;
  }

  Universe u;
  const auto uid = data_install(u, panel->frame);
  const auto under = u.get_underlying(*uid);
  const Underlying* U = *under;

  SurfaceParityInputs in{};
  in.S = panel->implied_spot;
  in.r = spec.r;
  in.now_ts_ns = panel->frame.snapshot_ts_ns;
  in.band_k = 1.0;
  in.repair = CalendarRepair::None;
  // Fast-cold de-Am (matches the session Fast preset): fast ALO, loose IV tol,
  // single ATM borrow pair. MEASUREMENT of the direct-path speedup.
  in.deam.al_opts = al_fast_opts();
  in.deam.iv_tol = 1.0e-5;
  in.deam.n_atm = 1;
  CurveConfig cfg;  // default ConvexDense

  std::printf("board: %zu chains, S=%.2f\n", U->chains.size(), in.S);

  // 1) full fit_curve_surface end-to-end
  auto t0 = clk::now();
  auto rep = fit_curve_surface(*U, in, cfg);
  auto t1 = clk::now();
  std::printf("fit_curve_surface TOTAL: %.1f ms (%zu slices)\n", ms(t0, t1),
              rep ? rep->surface.n_slices() : 0);

  // 2) replicate the loop with per-phase timers
  double t_fwd = 0, t_obs = 0, t_fit = 0;
  std::size_t n_strk = 0, n_obs = 0, n_chain_fit = 0;
  std::function<double(double)> w_prev;  // no floor in the probe (isolate cost)
  for (const Chain& chain : U->chains) {
    const double T = chain.T;
    if (!(T > 0.0)) continue;
    auto a = clk::now();
    const auto d = resolve_chain_forward(chain, in.S, in.r, in.cash_divs,
                                         in.now_ts_ns, in.deam);
    auto b = clk::now();
    t_fwd += ms(a, b);
    if (!d) continue;
    const double F = d->forward;
    if (!(F > 0.0)) continue;
    const double df = std::exp(-in.r * T);
    n_strk += chain.n_strikes();

    auto c = clk::now();
    const auto obs = build_observations_european(chain, in.S, in.r, F, T, df, in.calib);
    auto e = clk::now();
    t_obs += ms(c, e);
    if (!obs || obs->obs.size() < 5) continue;
    n_obs += obs->obs.size();

    auto g = clk::now();
    auto slice = fit_slice_curve(cfg, obs->obs, F, T, df, w_prev);
    auto h = clk::now();
    t_fit += ms(g, h);
    if (slice) ++n_chain_fit;
  }
  std::printf("PHASE breakdown (no floor):\n");
  std::printf("  resolve_chain_forward : %8.1f ms\n", t_fwd);
  std::printf("  build_obs_european    : %8.1f ms  (%zu strikes -> %zu obs)\n",
              t_obs, n_strk, n_obs);
  std::printf("  fit_slice_curve       : %8.1f ms  (%zu slices)\n", t_fit,
              n_chain_fit);
  return 0;
}
