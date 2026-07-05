// spy_diag.cpp — per-slice fit-vs-data diagnostic for the real SPY OPRA surface.
//
// Loads the cached SPY OPRA cbbo-1m parquet, builds the session, and for every
// fitted slice reports market-vs-model dispersion so we can separate a DATA
// problem (jagged de-Am'd smile / stale quotes) from a FIT problem (clean smile
// the eSSVI misses). For a couple of adjacent slices it dumps the sorted smile.
//
// Market IV proxy: build_observations' Black-76 European inversion of the raw
// mid (for SPY index options the American early-exercise premium is tiny, so
// this closely tracks the de-Am'd smile the fit actually saw). Model IV: the
// fitted surface read at the same (K, T).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "atx/vol/arb.hpp"
#include "atx/vol/calib.hpp"
#include "atx/vol/opra_panel.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/universe.hpp"

using namespace atx::vol;

static double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch())
      .count();
}

int main(int argc, char** argv) {
  const std::string path = argc > 1
      ? argv[1]
      : "data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet";
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  auto panel = load_opra_cbbo_parquet(spec);
  if (!panel.has_value()) {
    std::fprintf(stderr, "load failed: %s\n", panel.error().message().c_str());
    return 1;
  }

  Universe u;
  const auto uid = data_install(u, panel->frame);
  const auto under = uid.has_value() ? u.get_underlying(*uid) : decltype(u.get_underlying(0)){};
  if (!uid.has_value() || !under.has_value()) {
    std::fprintf(stderr, "install failed\n");
    return 1;
  }

  // Shipped Accurate preset (wing-residual layer ON — the crash-wing lever),
  // unless "nowing" is passed to A/B against the raw backbone.
  SessionInputs in = make_session_inputs(FitPreset::Accurate, panel->implied_spot,
                                         spec.r, panel->frame.snapshot_ts_ns);
  if (argc > 2 && std::string(argv[2]) == "nowing") {
    in.calib.residual_disable = true;
  }
  const double t0 = now_ms();
  auto sess = VolaSession::from_frame(panel->frame, in);
  const double build_ms = now_ms() - t0;
  if (!sess.has_value()) {
    std::fprintf(stderr, "build failed: %s\n", sess.error().message().c_str());
    return 1;
  }
  const auto ctx = sess->expiries();
  std::printf("SPY: spot %.2f, %zu slices fit, Accurate build %.0f ms%s\n\n",
              panel->implied_spot, ctx.size(), build_ms,
              in.calib.residual_disable ? " (wing OFF)" : " (wing ON)");

  std::printf("%-8s %5s %8s %9s %9s %9s %9s %9s %9s\n", "T", "n", "atm_iv",
              "full_rmse", "core_rmse", "vw_rmse", "med_hsprd", "worstk", "worstres");
  const Underlying* U = *under;
  std::vector<double> liq_vw;  // vega-weighted rmse over LIQUID slices (T >= 1wk)
  std::vector<double> liq_core;
  for (std::size_t i = 0; i < ctx.size() && i < U->chains.size(); ++i) {
    const auto& chain = U->chains[i];
    const double T = ctx[i].T;
    const double F = ctx[i].forward;
    const double df = std::exp(-spec.r * T);
    const auto obs = build_observations(chain, F, T, df, CalibOpts{});
    if (!obs.has_value() || obs->obs.size() < 5) {
      std::printf("%-8.4f  (build_observations: too few)\n", T);
      continue;
    }
    // Market dispersion: RMS of (mkt_iv - local linear trend) — a cheap
    // roughness measure. Fit rmse: mkt_iv vs the surface model.
    std::vector<double> ks, ivs, hspr;
    for (const auto& o : obs->obs) {
      ks.push_back(o.k);
      ivs.push_back(o.sigma_mkt);
      hspr.push_back(o.spread > 0 && o.vega > 0 ? 0.5 * o.spread / o.vega : 0.0);
    }
    // Model rmse: full, near-money core (|k|<=0.4), and vega^2-weighted.
    double sr2 = 0.0, worst = 0.0, worstk = 0.0, atm_iv = 0.0, atm_absk = 1e9;
    double core2 = 0.0; std::size_t ncore = 0;
    double wsum = 0.0, wsr2 = 0.0;
    for (std::size_t j = 0; j < ks.size(); ++j) {
      const double K = F * std::exp(ks[j]);
      const double m = sess->iv(K, T);
      const double res = m - ivs[j];
      sr2 += res * res;
      const double vega = obs->obs[j].vega;
      const double w = (vega > 0.0) ? vega * vega : 0.0;
      wsum += w; wsr2 += w * res * res;
      if (std::fabs(ks[j]) <= 0.4) { core2 += res * res; ++ncore; }
      if (std::fabs(res) > std::fabs(worst)) { worst = res; worstk = ks[j]; }
      if (std::fabs(ks[j]) < atm_absk) { atm_absk = std::fabs(ks[j]); atm_iv = ivs[j]; }
    }
    const double fit_rmse = std::sqrt(sr2 / static_cast<double>(ks.size()));
    const double core_rmse = ncore ? std::sqrt(core2 / static_cast<double>(ncore)) : 0.0;
    const double vw_rmse = wsum > 0.0 ? std::sqrt(wsr2 / wsum) : 0.0;
    // Market roughness: second difference of iv sorted by k.
    std::vector<std::size_t> idx(ks.size());
    for (std::size_t j = 0; j < idx.size(); ++j) idx[j] = j;
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b){ return ks[a] < ks[b]; });
    double d2 = 0.0; std::size_t nd = 0;
    for (std::size_t j = 1; j + 1 < idx.size(); ++j) {
      const double v = ivs[idx[j-1]] - 2*ivs[idx[j]] + ivs[idx[j+1]];
      d2 += v*v; ++nd;
    }
    const double mkt_disp = nd ? std::sqrt(d2 / static_cast<double>(nd)) : 0.0;
    std::vector<double> hs = hspr; std::sort(hs.begin(), hs.end());
    const double med_h = hs.empty() ? 0.0 : hs[hs.size()/2];
    (void)mkt_disp;
    std::printf("%-8.4f %5zu %8.4f %9.5f %9.5f %9.5f %9.5f %9.4f %9.5f\n", T,
                ks.size(), atm_iv, fit_rmse, core_rmse, vw_rmse, med_h, worstk,
                worst);
    if (T >= 0.019) {  // exclude the ultra-short (< ~1 week) 0DTE/weekly regime
      liq_vw.push_back(vw_rmse);
      liq_core.push_back(core_rmse);
    }
  }

  // Aggregate over the LIQUID surface (T >= ~1 week): median vega-weighted RMSE
  // and the fraction of slices whose vega-weighted fit is within ~2 vol points.
  auto median = [](std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  std::size_t under2 = 0;
  for (double x : liq_vw) {
    if (x < 0.02) ++under2;
  }
  const auto cal = arb_check_calendar(sess->surface(), -1.5, 1.5, 25);
  std::printf("\n=== SOTA accuracy (liquid surface, T >= 1wk; %zu slices) ===\n",
              liq_vw.size());
  std::printf("  median vega-wtd RMSE : %.5f  (%.2f vol pts)\n", median(liq_vw),
              100.0 * median(liq_vw));
  std::printf("  median core RMSE|k|<.4: %.5f\n", median(liq_core));
  std::printf("  slices vw-RMSE < 2vp : %zu / %zu\n", under2, liq_vw.size());
  std::printf("  calendar arb (|k|<1.5): %s\n",
              (cal.has_value() && cal->empty()) ? "arb-free"
                                                : "violations present (use Robust)");
  std::printf(
      "  NOTE: SPY NBBO half-spreads are ~1 penny, so fraction-in-bid-ask is a\n"
      "  penny-tick metric (a ~0.4 vol-pt fit still lands outside); vega-weighted\n"
      "  vol RMSE is the meaningful accuracy gate. Ultra-short (<1wk) and deep\n"
      "  tails (|k|>1.5) are separate regimes, excluded from this aggregate.\n");
  return 0;
}
